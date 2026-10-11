/* KraOS 内核链接脚本（ld.lld）
 *
 * 通过 konan.properties 的 `kraosLinkerScript.kraos_kernel` 指向本文件，
 * 覆盖 KraOSLinkerScript 的内置默认脚本。
 *
 * 布局分两段：
 *
 *   [0x00100000, __boot_lma_end)        引导桩，低地址（VMA == LMA）
 *   [0xffffffff80000000, __kernel_end)   内核主体，高半区（LMA 接在引导桩之后）
 *
 * 为什么要拆：GRUB2 在 **32 位保护模式**下跳到 ELF 的 e_entry，所以 e_entry
 * （也就是 _start）必须落在一个 32 位代码能取到的低地址上。而内核主体要跑在高
 * 半区 —— 两件事只能靠「低半区一段一次性引导桩 + 引导桩建好映射后跳过去」
 * 同时满足。GRUB 按 p_paddr 装载，所以高 VMA 的段仍然被装到低处的物理地址上。
 *
 * 低半区基址取 16 MiB —— 与 ArkOS 同一个理由：1 MiB 以下要避开传统 BIOS 的
 * EBDA 与显卡区域，8~9 MiB 附近则有固件保留区。
 *
 * 三条必须同时守住的约束（拆开任一条都会以「莫名其妙的三重故障」收场）：
 *
 *  1. rel32 的可达范围只有 ±2 GiB，而高低两段相距整整 4 GiB。因此**所有可被
 *     调用的函数都在高半区**，低半区引导桩不调用任何高半区符号；高半区代码读
 *     低半区数据一律用 movabsq 取绝对地址（boot/entry.S 里有对应注释）。
 *
 *  2. 引导桩与内核主体必须是**两组独立的 PT_LOAD**。同一个 PT_LOAD 里同时装低
 *     地址的 .data.boot 和高半区的 .text，段的地址区间就横跨 2 GB，lld 算出的
 *     段大小直接溢出，报一句完全看不出成因的
 *         output file too large: 18446744071545620856 bytes
 *
 *  3. 用了 AT() 做 LMA/VMA 分离之后，**每个输入段都必须显式归位**。孤儿段不认
 *     __lma 计数器，lld 给它算出的文件偏移会溢出成负数，报的还是上面那句。
 *
 * 另外，VMA 侧与 LMA 侧的 ALIGN() 必须同步：__kernel_lma 对齐到 2 MiB，于是
 * 固定平移量 D = KERNEL_VMA - __kernel_lma 对 4096 和 8 都整除。这样每段
 * ALIGN(n) 在 VMA 侧补的零和 LMA 侧补的零一样多，整段「VMA - LMA == D」恒等
 * 成立，引导桩才能简单地按 i 个 2 MiB 大页平铺映射整个内核。
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_VMA = 0xffffffff80000000;

PHDRS
{
    boot_text   PT_LOAD FLAGS(5);   /* 引导桩 R E，低地址 */
    boot_rodata PT_LOAD FLAGS(4);   /* 引导桩 R，低地址 */
    boot_data   PT_LOAD FLAGS(6);   /* 引导桩 RW（含 NOLOAD 的页表/栈/TLS/静态堆） */
    text   PT_LOAD FLAGS(5);   /* 内核主体 R E，高半区 */
    rodata PT_LOAD FLAGS(4);   /* 内核主体 R，高半区 */
    data   PT_LOAD FLAGS(6);   /* 内核主体 RW，高半区 */
    /* 一旦显式声明 PHDRS，ld.lld 就不再自动生成 PT_TLS，.tdata/.tbss 会失去
       SHF_TLS 标记并让链接失败。K/N 运行时用到 __thread（pthread_key_* 与
       errno 都依赖它），所以这里必须自己声明。 */
    tls    PT_TLS  FLAGS(4);
}

SECTIONS
{
    /* ---------------- 引导桩（低地址） ---------------- */
    . = 16M;
    __boot_start = .;

    /* Multiboot2 头必须落在可加载段内，且不能被 --gc-sections 丢掉。 */
    .multiboot ALIGN(8) : { KEEP(*(.multiboot)) } :boot_text

    /* .text.boot 是启动桩：32 位起步，切长模式后才轮到它之外的一切。
       必须排在最前，否则 GRUB 跳进来时还没进长模式就执行了长模式代码。 */
    .text.boot ALIGN(4096) : { KEEP(*(.text.boot)) } :boot_text
    .rodata.boot ALIGN(4096) : { *(.rodata.boot) } :boot_rodata
    .data.boot ALIGN(4096) : { *(.data.boot) } :boot_data

    /* 引导桩自己的页表、栈、TLS 块、静态堆。NOLOAD，不占文件空间，由
       clear_bss32 在 32 位阶段清零。必须待在低地址 —— 切到高半区之前就要用，
       而那时高半区的映射还不存在。 */
    .bss.boot ALIGN(2097152) (NOLOAD) :
    {
        __bss_boot_start = .;
        *(.bss.boot)
        . = ALIGN(4096);
        __bss_boot_end = .;
    } :boot_data

    __boot_lma_end = .;

    /* 内核主体的物理基址。大页的物理基址必须 2 MiB 对齐，所以这里先对齐；
       对齐之后 KERNEL_VMA 与 __kernel_lma 都 2 MiB 对齐，固定平移量
       D = KERNEL_VMA - __kernel_lma 对 4096 / 8 都整除，下面每段 ALIGN() 才能
       在 VMA 侧与 LMA 侧同步生效。中间空出来的不足 2 MiB 就当对齐空洞。 */
    __kernel_lma = (__boot_lma_end + 0x1fffff) & ~0x1fffff;

    /* ---------------- 内核主体（高半区） ---------------- */
    . = KERNEL_VMA;
    __kernel_start = .;

    /*
     * LMA 计数器。
     *
     * 写成 AT(LOADADDR(前一段) + SIZEOF(前一段)) 看着更直白，但一旦中间夹一个
     * NOLOAD 段（.tbss），后面几段就会拿到同一个基址而互相重叠。单调累加不会
     * 出这个问题。
     *
     * 关键：NOLOAD 段（.tbss/.bss）**不推进** __lma。lld 是按文件偏移装载的，
     * 一旦 NOLOAD 段在 LMA 上留出空洞，后面那个 PROGBITS 段的字节就会被装到
     * 与它自称的地址不同的物理位置上去。
     */
    __lma = __kernel_lma;

    .text ALIGN(4096) : AT(__lma)
    {
        *(.text .text.*)
        /* .init/.fini 是 C 运行时的入口/收尾；我们不跑它们，但它们可加载且可执行，
           放任不管就成了孤儿段（见文件头约束 3）。 */
        *(.init .fini)
        *(.iplt)
    } :text
    __lma = (__lma + SIZEOF(.text) + 4095) & ~4095;

    /* .gcc_except_table* 必须显式收进来：来自 C++ 代码（K/N 运行时是 C++ 写的），
       原脚本没写它们，ld 会当孤儿段自动安置。 */
    .rodata ALIGN(4096) : AT(__lma)
    {
        *(.rodata .rodata.*)
        *(.gcc_except_table .gcc_except_table.*)
    } :rodata
    __lma = (__lma + SIZEOF(.rodata) + 4095) & ~4095;

    /* 线程局部存储。.tdata 进镜像，.tbss 只占空间。
       内核自己在 P3 建起调度器之前只跑一个线程，但 K/N 运行时的 pthread_key_*
       依赖这段存在。

       .tbss 必须紧跟在 .tdata 后面，不能挪到整个高半区的末尾，原因有两条，
       都是踩出来的：

       1. PT_TLS 的 memsz 是「.tdata 的起点到 .tbss 的终点」。把 .tbss 放到最后
          意味着中间的 .init_array / .eh_frame / .data 全被算进这块 TLS，memsz
          会涨到十几 KiB。而引导桩给的 %fs 块只有 64 KiB（FS 基址还取在块中部），
          一旦有 __thread 变量落在块外就是 #PF。

       2. 更致命的是 GRUB：它按每个 PT_LOAD 的 [p_paddr, p_paddr + p_memsz) 向
          引导器申请一块低内存「保留区」，块边界要按页向上取整。.tbss 归 :rodata
          段，而 rodata 段的 memsz 又由段内**最高地址**的成员决定 —— .tbss 挪到
          末尾就会把整段 rodata 的跨度撑到几乎整个内核，于是它申请到的保留区盖住
          了后面 data 段的保留区，GRUB 直接报
              error: lib/relocator.c:grub_relocator_alloc_chunk_addr: overlap detected
          然后拒绝加载内核（屏幕上只有一句 "you need to load the kernel first"）。

       .tbss 是 NOLOAD，不推进 __lma，所以中间留的这个洞不会破坏
       「文件偏移 - p_paddr」的自洽：后面 .init_array 的字节仍然紧接 .tdata 装载。 */
    .tdata ALIGN(4096) : AT(__lma) { *(.tdata .tdata.*) } :rodata :tls
    __lma = (__lma + SIZEOF(.tdata) + 4095) & ~4095;
    .tbss ALIGN(4096) (NOLOAD) : { *(.tbss .tbss.*) *(.tcommon) } :rodata :tls

    /* Kotlin 全局对象与顶层属性的构造/析构函数表。必须落在可加载段 ——
       K/N 运行时在进入 main() 之前会遍历它们。 */
    .init_array ALIGN(4096) : AT(__lma)
    {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array .ctors))
        KEEP(*(.preinit_array))
        __init_array_end = .;
    } :rodata
    __lma = (__lma + SIZEOF(.init_array) + 4095) & ~4095;

    .fini_array ALIGN(4096) : AT(__lma)
    {
        __fini_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.fini_array .dtors))
        __fini_array_end = .;
    } :rodata
    __lma = (__lma + SIZEOF(.fini_array) + 4095) & ~4095;

    /* 保留 unwind 表：K/N 的 Throwable / try-catch 依赖它，unwind 器
       （libgcc_eh.a）会遍历这些表。 */
    .eh_frame ALIGN(4096) : AT(__lma) { KEEP(*(.eh_frame)) *(.eh_frame_hdr) } :rodata
    __lma = (__lma + SIZEOF(.eh_frame) + 4095) & ~4095;

    .data ALIGN(4096) : AT(__lma)
    {
        *(.data .data.*)
        *(.data.rel.ro .data.rel.ro.*)
        *(.sdata .sdata.*)
        *(.jcr)
        *(.tm_clone_table)
    } :data
    __lma = (__lma + SIZEOF(.data) + 4095) & ~4095;

    .got ALIGN(4096) : AT(__lma) { *(.got.plt) *(.got) } :data
    __lma = (__lma + SIZEOF(.got) + 4095) & ~4095;

    /* IRELATIVE（ifunc）重定位表。lld 遇到 ifunc（memcpy、strlen、memmove 这类
       多版本实现）只在链接期生成 .iplt 桩，桩体是一条「jmp *GOT 槽」，GOT 槽留空
       等运行时填。正常的 glibc 静态程序由 LIBC_START_MAIN 调 __libc_csu_irel 完成
       这一步；我们直接跳进 Konan_main，所以必须把表留下来，由引导代码调
       kraos_apply_irel() 逐条执行解析器。少了这张表，第一次走 ifunc（比如 libc++
       算 rehash 容量时的 ceil）就会跳进地址 0。 */
    .rela.iplt ALIGN(4096) : AT(__lma)
    {
        __rela_iplt_start = .;
        KEEP(*(.rela.iplt))
        __rela_iplt_end = .;
    } :data
    __lma = (__lma + SIZEOF(.rela.iplt) + 4095) & ~4095;

    /* .bss 是最后一个成员：不占文件空间，也不推进 __lma。
     *
     * 它排在文件内容之后，kraos_high_start 就没地方清零它（高半区的物理位置
     * 在镜像之后，此刻还没有任何映射）。引导桩因此按 __kernel_end 而不是
     * __kernel_lma_end 决定大页数量，把这一段也映射进来。 */
    .bss ALIGN(4096) (NOLOAD) :
    {
        __bss_start = .;
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(4096);
        __bss_end = .;
    } :data

    __kernel_end = .;
    __kernel_lma_end = __lma;
    _end = .;
    PROVIDE(end = .);

    /* 引导桩按这个数铺 2 MiB 大页。覆盖到 __kernel_end（含 .bss），而不只是
       覆盖到文件末尾。 */
    __kernel_pde_count = (__kernel_end - KERNEL_VMA + 0x1fffff) >> 21;

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        /* LLVM 在非 PIC 目标上会为函数地址生成这张表，本内核用不到 */
        *(.llvm_addrsig)
        *(.stab .stab.*)
        *(.debug .debug.*)
        /* freestanding：没有动态链接器，也就没有 .interp / .dynamic / PLT */
        *(.interp)
        *(.dynamic)
        *(.dynsym)
        *(.dynstr)
        *(.hash)
        *(.gnu.hash)
        *(.gnu.version .gnu.version.*)
        *(.rela .rela.*)
        *(.plt .plt.*)
    }
}
