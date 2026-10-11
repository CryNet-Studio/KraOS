# KraOS 内核构建。
#
# 与 ArkOS 的 Makefile 结构对齐（all/iso/run/check），差别只在编译内核用的是
# Kotlin/Native 的 konanc 而不是 gcc。
#
#   make            构建可引导 ISO
#   make kernel     只链接出内核 ELF
#   make run        在 QEMU 里启动，看串口输出
#   make check      静态校验（multiboot2 头、入口、段）
#   make clean

SHELL := /bin/bash

KONANC   ?= /home/rj/KraOS/.tooling/kotlin-native-linux-x86_64-2.1.21/bin/konanc
KORINTEROP ?= /home/rj/KraOS/.tooling/kotlin-native-linux-x86_64-2.1.21/bin/cinterop
TARGET ?= kraos_kernel
ROOT   := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD  := $(ROOT)/build

# 引导桩用宿主 gcc/as 汇编 —— 这段必须是纯机器码，Kotlin 生成不了 lgdt/lidt。
AS      := gcc
# libc 垫片用同样这组旗标编 gcc：freestanding、无 PIC、无 unwind 表。
CC      := gcc
ASFLAGS := -m64 -ffreestanding -fno-pie -fno-asynchronous-unwind-tables
# -fno-builtin：ifunc_shim.c 里的循环不能被 gcc 折回对 memcpy/memset 的调用
# -mcmodel=kernel：内核主体在 0xffffffff80000000，默认的 small 模型会用
#   R_X86_64_32（零扩展，只能表示 4 GiB 以下的地址）去寻址全局数据，高半区的
#   符号一律被截断。kernel 模型改用 R_X86_64_32S（符号扩展），高半区与低半区的
#   绝对地址都能正确表示。K/N 自己的位码是 pic 模式（GOT 取址），不受影响。
# -ffunction-sections/-fdata-sections：KraOSLinker 已无条件启用 --gc-sections；
#   没有这两个旗标时，本文件里所有 C 函数都挤在单个 .text 段，链接器无法逐函数
#   回收死实现（此前实测给 lld 加 GC 却零收益，根因就在这里）。
CCFLAGS := -m64 -ffreestanding -fno-builtin -fno-pie -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -O2 -mcmodel=kernel

KOTLIN_SOURCES := $(shell find $(ROOT)/kernel/src -name '*.kt' 2>/dev/null)

# -Xbinary=gc=noop 关掉内置 GC；allocationMode 默认已经是 CUSTOM，无需显式指定。
#   理由：内置 GC 需要 stop-the-world + 线程挂起 + 可重 relocating 的堆，
#   这三样在调度器（P3）落地之前都不存在。P4 再用 Kotlin 自研 mark-sweep 接管。
# C 兼容实现按函数拆段并由 KraOSLinker 的 --gc-sections 裁剪；这里仅保留
# 当前 K/N 运行时确实引用的 ifunc 替代项。其余 wrapper 与源实现已由最终 ELF
# 的 section-GC 结果证明不可达，避免继续维护未用接口。
LINK_WRAPS := \
	--wrap=bcmp --wrap=memcpy --wrap=memmove --wrap=memset \
	--wrap=strcmp --wrap=strlen --wrap=strncpy --wrap=strrchr \
	--wrap=ceil --wrap=floor

KONAN_FLAGS := -target $(TARGET) -Xbinary=gc=noop -opt \
	$(addprefix -linker-option=,$(LINK_WRAPS))

ENTRY_OBJ := $(BUILD)/entry.o
LIBC_SHIM := $(BUILD)/libc_shim.o
IFUNC_SHIM := $(BUILD)/ifunc_shim.o
LIBC_MIN   := $(BUILD)/libc_min.o
BOOT_KLIB := $(BUILD)/boot.klib
KERNEL    := $(BUILD)/kernel.elf
ISO       := $(BUILD)/kraos.iso

.PHONY: all kernel iso run check clean

all: iso

build:
	@mkdir -p $(BUILD)

$(ENTRY_OBJ): boot/entry.S | build
	$(AS) $(ASFLAGS) -c $< -o $@

# libc 分配入口。必须用 gcc 编成真实强符号：Kotlin 的 @CName 导出对
# 「K/N 运行时没引用过的符号」不会生成全局定义（见 boot/libc_shim.c 注释）。
$(LIBC_SHIM): boot/libc_shim.c | build
	$(CC) $(CCFLAGS) -c $< -o $@

# 顶掉 glibc ifunc 的自由 str/mem 实现，靠 --wrap 生效。
$(IFUNC_SHIM): boot/ifunc_shim.c | build
	$(CC) $(CCFLAGS) -c $< -o $@

# P2：顶替被摘掉的 glibc 归档。必须用 C —— Kotlin 的 @CName 只有在运行时位码
# 本身引用了该符号时才导出全局定义，这 20 个符号里没有一个能被 Kotlin 侧引用到。
$(LIBC_MIN): boot/libc_min.c | build
	$(CC) $(CCFLAGS) -c $< -o $@

# 启动桩的 C ABI 绑定。走 cinterop 而不是 @GCUnsafeCall/@SymbolName：后两者
# 在 2.1.x 要么被禁用，要么要求一个 internal 的 opt-in 标记，用户代码引用不了。
$(BOOT_KLIB): boot/boot.def | build
	$(KORINTEROP) -target $(TARGET) -Xdisable-experimental-annotation -def $< -o $(BUILD)/boot

$(KERNEL): $(ENTRY_OBJ) $(LIBC_SHIM) $(IFUNC_SHIM) $(LIBC_MIN) $(BOOT_KLIB) $(KOTLIN_SOURCES) boot/linker.ld | build
	@test -n "$(KOTLIN_SOURCES)" || { echo "找不到 kernel/src 下的 Kotlin 源码" >&2; exit 1; }
	cd $(BUILD) && $(KONANC) $(KONAN_FLAGS) \
		$(KOTLIN_SOURCES) \
		-library $(BOOT_KLIB) \
		-o kernel.kexe
	@# konanc 会按 -o 的名字产出 .kexe，统一改成 kernel.elf
	@mv -f $(BUILD)/kernel.kexe $(KERNEL)

kernel: $(KERNEL)

# GRUB 的 i386-pc 模块目录。eltorito.img 必须是
#   cdboot.img（CD 的 2048 字节引导扇区） + grub-mkimage 生成的 core image
# 而不是 grub-mkimage 直接产出的 boot.img —— 后者是给软盘/硬盘 MBR 用的，
# 放到 CD 上会取到一段乱码。
GRUBPC ?= /usr/lib/grub/i386-pc
GRUB_MODULES := biosdisk part_msdos iso9660 fat normal chain configfile \
                search search_fs_file multiboot2 serial terminal echo

$(BUILD)/iso/boot/grub/i386-pc/eltorito.img: boot/grub.cfg $(KERNEL) | build
	@rm -rf $(BUILD)/iso
	@mkdir -p $(BUILD)/iso/boot/grub/i386-pc
	cp $(KERNEL) $(BUILD)/iso/boot/kernel.elf
	cp boot/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	grub-mkimage -O i386-pc -p /boot/grub -o $(BUILD)/iso/boot/grub/i386-pc/core.img $(GRUB_MODULES)
	cat $(GRUBPC)/cdboot.img $(BUILD)/iso/boot/grub/i386-pc/core.img > $@

# 直接用 xorriso 拼 ISO，参数与 grub-mkrescue 内部那条命令行一致，
# 只是去掉了 EFI 那半边（那半边需要 mformat，这台机器上没有 mtools）。
# --grub2-boot-info 不能省：没有它 xorriso 不会写出正确的 El Torito 引导记录。
$(ISO): $(BUILD)/iso/boot/grub/i386-pc/eltorito.img
	xorriso -as mkisofs -R \
		-b boot/grub/i386-pc/eltorito.img -no-emul-boot \
		-boot-load-size 4 -boot-info-table \
		--grub2-mbr $(GRUBPC)/boot.img --grub2-boot-info \
		-o $@ $(BUILD)/iso

iso: $(ISO)

# 用 QEMU 图形窗口显示 Multiboot2 framebuffer；串口日志继续输出到当前终端。
run: $(ISO)
	qemu-system-x86_64 \
		-machine pc -cpu max -m 256M \
		-cdrom $(ISO) -boot d \
		-display gtk -vga std -serial stdio -nic none -no-reboot

# 验收检查：P1 的三条硬指标都要在这里被真的断言，而不是「打印出来看着像」。
#
# readelf 在中文 locale 下会把标签翻成「入口点」这类中文，grep -E 'Entry point'
# 就永远匹配不上（踩过一次：grep 失败被 make 当成失败，一度以为 ELF 有问题）。
# 凡是给 readelf/objdump 喂管道的地方都强制 LC_ALL=C。
check: $(KERNEL)
	@echo "== multiboot2 头 =="
	@grub-file --is-x86-multiboot2 $(KERNEL) && echo "  [OK] 是合法的 multiboot2 镜像"
	@echo "== ELF 概要 =="
	@LC_ALL=C readelf -hW $(KERNEL) | grep -E 'Type:|Machine:|Entry point'
	@echo "== 断言 1：无 PT_INTERP（裸机没有解释器） =="
	@if LC_ALL=C readelf -lW $(KERNEL) | grep -q INTERP; then \
	  echo "  [FAIL] 出现了 PT_INTERP"; exit 1; \
	else echo "  [OK] 没有 PT_INTERP"; fi
	@echo "== 断言 2：控制权确实交给了 Konan_main =="
	@entry=$$(LC_ALL=C readelf -hW $(KERNEL) | awk '/Entry point/{print $$NF}' | sed 's/0x//'); \
	 start=$$(nm $(KERNEL) | awk '$$3=="_start"{print $$1}'); \
	 main=$$(nm $(KERNEL) | awk '$$3=="Konan_main"{print $$1}'); \
	 if [ $$((0x$$entry)) -ne $$((0x$$start)) ]; then \
	   echo "  [FAIL] ELF 入口 $$entry 不是 multiboot2 入口 _start $$start"; exit 1; \
	 fi; \
	 if ! objdump -d --start-address=$$start $(KERNEL) 2>/dev/null \
	      | grep -q "call.*<Konan_main>"; then \
	   echo "  [FAIL] _start 里没有对 Konan_main 的调用"; exit 1; \
	 fi; \
	 echo "  [OK] ELF 入口 $$entry = _start（GRUB 必须落在 32 位入口），"; \
	 echo "       且 _start 在长模式里 call Konan_main ($$main)"

	@echo "== 断言 3：静态可执行、无动态链接器 =="
	@if LC_ALL=C readelf -lW $(KERNEL) | grep -qE 'INTERP|DYNAMIC'; then \
	  echo "  [FAIL] 仍有动态链接段"; exit 1; \
	else echo "  [OK] 无 INTERP / DYNAMIC"; fi
	@echo "== 段一览 =="
	@LC_ALL=C readelf -lW $(KERNEL) | grep -E 'LOAD|TLS|GNU_STACK'
	@echo "== 断言 4：链接产物里没有 ifunc 桩 =="
	@# 匹配整个节名：残留的 .rela.iplt 是空的（0 字节），不算 ifunc 桩。
	@if LC_ALL=C readelf -SW $(KERNEL) | grep -qE '[[:space:]]\.iplt[[:space:]]'; then \
	  echo "  [FAIL] 仍存在 .iplt（ifunc 未解析）"; exit 1; \
	else echo "  [OK] 无 .iplt 代码桩"; fi
	@LC_ALL=C readelf -SW $(KERNEL) | grep -E '\.rela\.iplt' \
	  | awk '{printf "       .rela.iplt 大小 = %s 字节\n", strtonum("0x" $$6)}'
	@echo "== 断言 5：链接布局可装载（GRUB 保留区不相交 / 高半区平移量恒定 / 大页对齐） =="
	@# GRUB 对每个 PT_LOAD 按 [p_paddr, p_paddr+p_memsz) 申请保留区、按页取整，
	@# 相交就报 "overlap detected" 然后拒载（串口全空，跟内核没跑一样）。
	@# 这里把那段模拟提前到链接后，省得每次都开 QEMU 才暴露。
	@if ! python3 tools/checklayout.py $(KERNEL) >/tmp/checklayout.out 2>&1; then \
	  cat /tmp/checklayout.out; echo "  [FAIL] checklayout 未过"; exit 1; \
	else echo "  [OK] 布局自洽（tools/checklayout.py，详见 /tmp/checklayout.out）"; fi
	@echo "== 关键符号 =="
	@nm $(KERNEL) | grep -wE '_start|Konan_main|__kernel_start|__bss_start|__bss_end|kraos_heap' || true


clean:
	rm -rf $(BUILD)