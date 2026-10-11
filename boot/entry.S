/* KraOS 内核启动桩。
 *
 * Multiboot2 以 32 位保护模式进入这个 ELF64 镜像。前半段（.text.boot）是 32 位
 * 代码：建长模式页表 -> 打开 PAE -> 置 EFER.LME -> 装载 GDT -> 开启分页 -> ljmp
 * 进长模式。然后由一个极短的 64 位「过桥」跳进高半区，后半段（.text）才是真正
 * 的 64 位内核代码，由它把控制权交给 Kotlin/Native 运行时。
 *
 * 为什么要分两段、为什么要过桥：
 *
 *   GRUB2 在 **32 位保护模式**下跳到 ELF 的 e_entry，所以 e_entry（也就是 _start）
 *   必须落在一个 32 位代码能取到的低地址上。可 P2 又要求内核主体跑在高半区
 *   0xffffffff80000000。两件事只能靠「低半区一段一次性引导桩 + 高半区内核主体」
 *   同时满足。
 *
 * 这条分界线还带出一条硬约束：**rel32 重定位的可达范围只有 ±2 GiB**，而低半区
 * 引导桩与高半区内核相距整整 4 GiB。因此
 *
 *   - 所有可被调用的函数（全部 kraos_* 导出、Konan_main、fault/serial 辅助）
 *     一律放高半区，高↔高调用距离 < 2 GiB，安全；
 *   - 低半区只留下 _start 及其 32 位助手，它们不调用任何高半区符号；
 *   - 唯一的跨界「调用」是过桥本身，用 movabsq 物化 64 位地址后间接跳转，
 *     绕开 rel32。
 *
 * 高半区代码若要读低半区的数据（kraos_heap、mb_magic、mb_info），也不能用
 * %rip 相对寻址（同样是 rel32），一律改成 movabsq 取绝对地址。
 */

/* K/N 运行时线程局部块：整块放在 .bss.boot 里，由 clear_bss32 清零。
   KRAOS_TLS_OFFSET 让 %fs:0 落在块中部，这样 -0x30 与 +0x6b0 两头都有空间。 */
.equ KRAOS_TLS_OFFSET, 0x1000
.equ KRAOS_TLS_SIZE,  0x10000

/* 2 MiB 大页粒度：高半区页表按它展开，映射数量由链接器算好传进来。 */
.equ PAGE_2M, 0x200000

/* ------------------------------------------------------------------ *
 *  Multiboot2 头                                                     *
 * ------------------------------------------------------------------ */
.section .multiboot,"a"
.balign 8
mb2_header:
    .long 0xe85250d6
    .long 0
    .long mb2_header_end-mb2_header
    .long -(0xe85250d6 + (mb2_header_end-mb2_header))
    /* 索要的信息项：内存图 + RGB framebuffer */
    .short 1, 1
    .long 16
    .long 6, 8
    /* 帧缓冲偏好值；GRUB 通常会给最接近的模式，内核启动时会自行校验 */
    .short 5, 0
    .long 20
    /* Ask GRUB for a full-HD desktop mode when the display driver offers it. */
    .long 1920, 1080, 32
    .balign 8
    .short 0, 0
    .long 8
mb2_header_end:

/* ================================================================== *
 *  低半区引导桩（32 位起步 + 一个 64 位过桥）                          *
 * ================================================================== */
.section .text.boot,"ax"
.code32

/* 逐张清零一张 4 KiB 页表。gas 的宏必须在使用之前定义。 */
.macro ZERO_PAGE sym
    movl $\sym, %edi
    xorl %eax, %eax
    movl $(4096/4), %ecx
    rep stosl
.endm

.global _start
.type _start,@function
_start:
    cli
    cld
    /* 仍在 32 位模式：%rip 寻址不可用，低半区符号的绝对地址又都在 4 GiB
       以内（R_X86_64_32 直接可用），所以这里用绝对寻址。 */
    movl %eax, mb_magic
    movl %ebx, mb_info
    /* 清 .bss.boot 必须在切到自己的栈之前做：引导栈就在 .bss.boot 里，
       而 clear_bss32 的 ret 还要从当前栈上取返回地址。这里仍用 GRUB 的栈，
       它不在本镜像的 .bss 范围内。而且此时还没开分页，写入地址就是物理地址。 */
    call clear_bss32
    movl $boot_stack_top, %esp
    call serial_init32
    movl $msg_entry, %esi
    call serial_puts32

    /* 先确认 CPU 真的支持 long mode，再去动 MSR。 */
    pushfl
    popl %eax
    movl %eax, %ecx
    xorl $0x00200000, %eax
    pushl %eax
    popfl
    pushfl
    popl %eax
    pushl %ecx
    popfl
    xorl %ecx, %eax
    testl $0x00200000, %eax
    jz boot_no_long_mode
    movl $0x80000000, %eax
    cpuid
    cmpl $0x80000001, %eax
    jb boot_no_long_mode
    movl $0x80000001, %eax
    cpuid
    testl $0x20000000, %edx
    jz boot_no_long_mode
    movl $msg_lm_ok, %esi
    call serial_puts32

    /* 清空八张页表（PML4、PDPT、4 张页目录、hi_pdpt、hi_pd）。
       逐张清而不是一块 rep stosl —— 一旦日后往中间插一个 .balign，块状写法
       就会漏清后面那张，而漏清的页表项表现为「莫名其妙的三重故障」。 */
    ZERO_PAGE pml4
    ZERO_PAGE pdpt
    ZERO_PAGE page_directories
    ZERO_PAGE hi_pdpt
    ZERO_PAGE hi_pd

    /* --- 低半区：PML4[0] -> pdpt -> 4 张页目录，线性映射 [0, 4 GiB) --- */
    movl $pdpt, %eax
    orl  $3, %eax
    movl $pml4, %edi
    movl %eax, (%edi)

    movl $page_directories, %eax
    orl  $3, %eax
    movl $pdpt, %edi
    movl $4, %ecx
1:
    movl %eax, (%edi)
    addl $4096, %eax
    addl $8, %edi
    loop 1b
    /* 2048 个 2 MiB 大页，线性映射物理地址 [0, 4 GiB)。 */
    movl $page_directories, %edi
    movl $0x83, %eax
    movl $2048, %ecx
2:
    movl %eax, (%edi)
    addl $0x200000, %eax
    addl $8, %edi
    loop 2b

    /* --- 高半区：PML4[511] -> hi_pdpt[510] -> hi_pd[i] -> 内核镜像 ---
     *
     * 0xffffffff80000000 的 48 位线性地址是 0xffff80000000，拆成
     *   PML4 下标 511 / PDPT 下标 510 / PD 下标 0
     * 所以要在原本 6 张页表之外另加 hi_pdpt 与 hi_pd 两张。
     *
     * 大页的物理基址必须 2 MiB 对齐，所以 __kernel_lma 在链接脚本里就对齐到
     * PAGE_2M；此时「内核 VMA 基址」与「内核 LMA 基址」都 2 MiB 对齐，
     * 整段 VMA→LMA 就是同一个固定平移量 D，按 i 个大页平铺即可。
     * 映射页数覆盖到 __kernel_end（而不只是文件末尾），因为高半区的 .bss
     * 排在文件内容之后、同样需要通过这张映射才能访问和清零。
     */
    movl $hi_pdpt, %eax
    orl  $3, %eax
    movl $pml4, %edi
    movl %eax, 511*8(%edi)

    movl $hi_pd, %eax
    orl  $3, %eax
    movl $hi_pdpt, %edi
    movl %eax, 510*8(%edi)

    movl $__kernel_lma, %eax
    orl  $0x83, %eax
    movl $hi_pd, %edi
    movl $__kernel_pde_count, %ecx
3:
    movl %eax, (%edi)
    movl $0, 4(%edi)             /* 物理地址 < 4 GiB，高 32 位为 0 */
    addl $0x200000, %eax
    addl $8, %edi
    loop 3b

    movl %cr4, %eax
    orl  $0x20, %eax                 /* CR4.PAE */
    movl %eax, %cr4
    movl $pml4, %eax
    movl %eax, %cr3
    movl $0xc0000080, %ecx          /* IA32_EFER */
    rdmsr
    orl  $0x100, %eax               /* EFER.LME */
    wrmsr
    lgdt gdt_pointer
    movl %cr0, %eax
    orl  $0x80000001, %eax          /* CR0.PG | CR0.PE */
    movl %eax, %cr0
    movl $msg_paging, %esi
    call serial_puts32
    ljmp $0x08, $long_mode_entry

boot_no_long_mode:
    movl $msg_cpu_error, %esi
    call serial_puts32
4:
    cli
    hlt
    jmp 4b

serial_init32:
    movw $0x3f9, %dx
    xorb %al, %al
    outb %al, %dx
    movw $0x3fb, %dx
    movb $0x80, %al
    outb %al, %dx
    movw $0x3f8, %dx
    movb $1, %al
    outb %al, %dx
    movw $0x3f9, %dx
    xorb %al, %al
    outb %al, %dx
    movw $0x3fb, %dx
    movb $3, %al
    outb %al, %dx
    movw $0x3fa, %dx
    movb $0xc7, %al
    outb %al, %dx
    movw $0x3fc, %dx
    movb $0x0b, %al
    outb %al, %dx
    ret
serial_puts32:
    lodsb
    testb %al, %al
    jz 6f
    movb %al, %bl
    movl $100000, %ecx
    movw $0x3fd, %dx
5:
    inb %dx, %al
    testb $0x20, %al
    jnz 7f
    loop 5b
7:
    movw $0x3f8, %dx
    movb %bl, %al
    outb %al, %dx
    jmp serial_puts32
6:
    ret

/* 低半区 .bss 清零：[__bss_boot_start, __bss_boot_end)。
   这段全部待在 4 GiB 以内（页表、引导栈、TLS 块、静态堆），32 位绝对寻址够用。
   高半区的 .bss 不归这里管 —— 它的物理位置在高 LMA 之后，而那部分内存在
   此刻还没有任何映射，由 kraos_high_start 建好高半区映射之后自己清。 */
clear_bss32:
    movl $__bss_boot_start, %edi
    movl $__bss_boot_end, %ecx
    subl %edi, %ecx
    xorl %eax, %eax
    cld
    rep stosb
    ret

/* 64 位过桥。刻意保持得极短：这里仍然只碰低半区的东西（栈、GDT、串口），
   不调用任何高半区符号 —— 一旦调用，call 的 PLT32 就会因为跨越 4 GiB 而越界。 */
.code64
long_mode_entry:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xorw %ax, %ax
    movw %ax, %fs
    movw %ax, %gs
    leaq boot_stack_top(%rip), %rsp
    xorq %rbp, %rbp

    /* 过桥。call/jmp *sym 走 PLT32（rel32），跨 4 GiB 无效，所以先把 64 位
       目标地址用 movabsq（R_X86_64_64）物化出来，再间接跳转。 */
    movabsq $kraos_high_start, %r11
    jmp    *%r11

/* ================================================================== *
 *  高半区内核主体（0xffffffff80000000）                                *
 * ================================================================== */
.section .text,"ax"
.code64
.global kraos_high_start
.type kraos_high_start,@function
kraos_high_start:
    /* 高半区 .bss 不占文件空间，先自己清零。
       必须在 kraos_setup_idt 之前：那张静态 IDT 就住在 .bss 里，
       表项没清零就 lgdt，等于把 CPU 指向一段全 0 的门描述符。 */
    leaq __bss_start(%rip), %rdi
    leaq __bss_end(%rip), %rcx
    subq %rdi, %rcx
    xorq %rax, %rax
    cld
    rep stosb

    /* 串口提示语放在高半区 .rodata，低半区那条搬过来时改用 movabsq 取绝对地址。 */
    movabsq $msg_lm, %rsi
    call serial_puts64

    /*
     * 打开 SSE / AVX 的状态。
     *
     * x86-64 架构上 SSE2 是强制基线，CR0.EM 默认为 0，所以**不装 IDT 之前
     * SSE 状态本来就是可用的**。真正要做的是把 CR0.MP 置上，让 x87/SSE 与
     * 其他处理器的状态协调起来；顺带把 CR4.OSFXSR 打开，FXSAVE/FXRSTOR 才生效。
     *
     * XSAVE 那一整套（CR4.OSXSAVE -> xsetbv）是**可选的**，只有 CPU 报告了
     * XSAVE/OSXSAVE 才碰。这里踩过一个很隐蔽的坑：
     *
     *   CR4 的 bit 18（OSXSAVE）在没有 XSAVE 的处理器上是**保留位**，
     *   写它直接 #GP。而此刻 IDT 还没装，#GP 会变成三重故障、机器静默复位，
     *   串口上只留下「已进入 64 位长模式」然后就没了 —— 看起来像内核死了，
     *   实际是引导桩自己踩了未定义行为。QEMU 的默认 CPU 模型 qemu64 就没有
     *   XSAVE，所以不显式加 -cpu max 就必然踩到。
     *
     * XCR0 的位同样只能开 CPU 真的有的：x87|SSE 是 0x3 永远安全，
     * AVX(YMM) 还要 CPUID.1:ECX[28] 才允许置位。
     *
     *   CPUID.1:ECX[26] = XSAVE， [27] = OSXSAVE， [28] = AVX
     */
    movl $0x3, %r8d              /* XCR0 默认 = x87 | SSE */

    movl $1, %eax
    xorl %ecx, %ecx
    cpuid
    testl $(1 << 27), %ecx       /* OSXSAVE：允许写 CR4.OSXSAVE */
    jz    .Lno_xsave
    testl $(1 << 26), %ecx       /* XSAVE：xsetbv 可用 */
    jz    .Lno_xsave
    testl $(1 << 28), %ecx       /* AVX：YMM 状态域存在 */
    jz    .Lno_avx
    movl $0x7, %r8d              /* x87 | SSE | AVX */
.Lno_avx:
    movq %cr4, %rax
    orq  $(1 << 18), %rax        /* CR4.OSXSAVE */
    movq %rax, %cr4
    movl %r8d, %eax
    xorl %edx, %edx
    xorl %ecx, %ecx              /* XCR0 索引 */
    xsetbv
.Lno_xsave:
    /* 控制寄存器只能读进 64 位寄存器，所以一律走 %rax。 */
    movq %cr4, %rax
    orq  $(1 << 9), %rax         /* CR4.OSFXSR，FXSAVE/FXRSTOR 生效 */
    movq %rax, %cr4
    movq %cr0, %rax
    orq  $(1 << 1), %rax         /* CR0.MP，SSE 与其他处理器的协调位 */
    movq %rax, %cr0
    fninit                      /* 初始化 x87 控制字 */

    /*
     * 设置 FS 基址（线程局部存储）。
     *
     * K/N 运行时的入口第一件事就是 `mov %fs:0x0,%rax`，把 %fs:0 当作指向
     * 自身线程状态的指针，随后访问 -0x30 .. +0x6b0 这一段相对偏移。裸机上
     * 没有 C 运行时的 __libc_setup_tls，FS 基址默认是 0，于是第一条访存就
     * 拿回一段垃圾并触发 #GP -> 三重故障。这里给它一块自己清零过的 64 KiB
     * 区域，FS 基址取块中部：向下的负偏移与向上的正偏移都落在块内。
     *
     * 该块住在低半区（.bss.boot），而当前代码在高半区，所以必须 movabsq 取
     * 绝对地址 —— %rip 相对寻址跨不过 4 GiB。FS 基址本身落在低半区的身份
     * 映射里，不需要为它单独建映射。
     *
     * 不使用 wrfsbase —— 那条指令要 CR4.FSGSBASE，这里是靠 WRMSR 直写
     * IA32_FS_BASE（0xC0000100），内核态下始终可用。
     */
    movabsq $kraos_tls_block, %rax
    addq $KRAOS_TLS_OFFSET, %rax
    movq  %rax, %rdx
    shrq  $32, %rdx
    movl  %eax, %eax               /* eax = 低 32 位 */
    movl  $0xC0000100, %ecx        /* IA32_FS_BASE */
    wrmsr
    movq  %rax, %fs:0              /* %fs:0 = 线程状态指针 */

    /* 建并装载 IDT。之后任何异常都会被 fault_common 接住并打印，而不是三重故障。 */
    call kraos_setup_idt

    /*
     * 装上系统调用入口。
     *
     * K/N 运行时里有一批「直接下 syscall 指令」的包装（gettid、futex、
     * clock_gettime……），它们不是普通的函数调用，链接期没有任何符号可以
     * wrap，只能真的接住这条指令。syscall 指令的目标地址由 IA32_LSTAR 指定，
     * 段选择子由 IA32_STAR 指定（CS = STAR[47:32]，SS = CS + 8，正好对上
     * 我们的 GDT 里 0x08/0x10 两项）。
     */
    movl $0xC0000080, %ecx             /* IA32_EFER */
    rdmsr
    orl  $0x1, %eax                   /* EFER.SCE：不开这个位 syscall 本身是 #UD */
    wrmsr

    movl $0xC0000081, %ecx             /* IA32_STAR */
    /* STAR[47:32] 给 syscall 选 CS（=0x08，SS 取 CS+8=0x10）；
     * STAR[63:48] 给 sysretq 选 CS（+16 = 0x18）、SS（+8 = 0x10）。 */
    movabsq $0x0008000800000008, %rax
    movq %rax, %rdx
    shrq $32, %rdx
    movl %eax, %eax
    wrmsr

    movl $0xC0000082, %ecx             /* IA32_LSTAR */
    movabsq $syscall_entry, %rax
    movq %rax, %rdx
    shrq $32, %rdx
    movl %eax, %eax
    wrmsr

    movl $0xC0000084, %ecx             /* IA32_SFMASK */
    movabsq $0x600, %rax               /* 进入时强制 IF=0、TF=0 */
    movq %rax, %rdx
    shrq $32, %rdx
    movl %eax, %eax
    wrmsr

    /* 先把 ifunc 的 IRELATIVE 重定位落实，否则 memcpy 这类多版本实现的
     * 调用桩会跳到空地址。之后才是 Kotlin/Native 运行时。 */
    call kraos_apply_irel

    /* 移交 Kotlin/Native 运行时：Konan_main(argc=0, argv=NULL)。
       它内部会做 Kotlin_initRuntimeIfNeeded()，而那一路分配最终会打到
       我们提供的 mmap 上 —— 所以本文件里的静态堆必须已经可用。 */
    movabsq $msg_konan, %rsi
    call serial_puts64
    xorl %edi, %edi
    xorl %esi, %esi
    call Konan_main
    movabsq $msg_konan_done, %rsi
    call serial_puts64

    /* Kotlin 返回即视为内核结束（正常情况下不会走到这里）。 */
    movabsq $msg_halt, %rsi
    call serial_puts64
5:
    cli
    hlt
    jmp 5b

/* 长模式下的串口输出。rsi = 以 0 结尾的字符串。 */
serial_puts64:
    lodsb
    testb %al, %al
    jz 6f
    movb %al, %bl
    movw $0x3fd, %dx
    movl $100000, %ecx
7:
    inb %dx, %al
    testb $0x20, %al
    jnz 8f
    loop 7b
8:
    movb %bl, %al
    movw $0x3f8, %dx
    outb %al, %dx
    jmp serial_puts64
6:
    ret

/* --- 供 Kotlin 侧调用的引导桩接口 ---------------------------------------
 * 静态堆放在低半区 .bss.boot（引导期先用它顶替真正的物理页管理器；
 * P2 换成解析 Multiboot2 内存图的位图分配器）。
 * kraos_heap 与本函数分处高低两个区域，%rip 相对寻址够不着，必须 movabsq。
 * 返回值走 %rax，Kotlin 侧不需要对指针宽度做任何假设。
 */
.global kraos_arena_start
.type kraos_arena_start,@function
kraos_arena_start:
    movabsq $kraos_heap, %rax
    ret
.size kraos_arena_start, .-kraos_arena_start

.global kraos_arena_end
.type kraos_arena_end,@function
kraos_arena_end:
    movabsq $kraos_heap_end, %rax
    ret
.size kraos_arena_end, .-kraos_arena_end

/* Multiboot2 头与信息指针所在的物理/虚拟地址。同样在低半区。 */
.global kraos_mb_magic
.type kraos_mb_magic,@function
kraos_mb_magic:
    movabsq $mb_magic, %rax
    movl   (%rax), %eax
    ret
.size kraos_mb_magic, .-kraos_mb_magic

.global kraos_mb_info
.type kraos_mb_info,@function
kraos_mb_info:
    movabsq $mb_info, %rax
    movq   (%rax), %rax
    ret
.size kraos_mb_info, .-kraos_mb_info

/* --- 特权操作 -------------------------------------------------------
 * Kotlin/Native 2.1.255 这一版没有可用的内联汇编（asm / asParam 均无法解析），
 * 而 IO 端口读写和 cli/hlt/sti 在指令集层面根本没有 Kotlin 表达，所以这三样
 * 只能留在启动桩里，以 C ABI 暴露给 Kotlin。
 * SysV x86_64 约定：第一个整型参数在 %edi，第二个在 %esi，返回值在 %eax。
 *
 * 这些符号必须与调用方（Kotlin / K/N 运行时）同在高半区：call 走 PLT32，
 * 跨 4 GiB 会直接越界。
 */
.global kraos_outb
.type kraos_outb,@function
kraos_outb:
    movl %edi, %edx
    movl %esi, %eax
    outb %al, %dx
    ret
.size kraos_outb, .-kraos_outb

.global kraos_inb
.type kraos_inb,@function
kraos_inb:
    movl %edi, %edx
    xorl %eax, %eax
    inb %dx, %al
    /* inb 只写 %al，这里补齐高 24 位，保证 Kotlin 侧拿到的是 0..255 */
    movzbl %al, %eax
    ret
.size kraos_inb, .-kraos_inb

.global kraos_cli
.type kraos_cli,@function
kraos_cli:
    cli
    ret
.size kraos_cli, .-kraos_cli

.global kraos_sti
.type kraos_sti,@function
kraos_sti:
    sti
    ret
.size kraos_sti, .-kraos_sti

.global kraos_hlt
.type kraos_hlt,@function
kraos_hlt:
    hlt
    ret
.size kraos_hlt, .-kraos_hlt

/* 时间戳计数器，供 clock_gettime 用。 */
.global kraos_rdtsc
.type kraos_rdtsc,@function
kraos_rdtsc:
    rdtsc
    shlq $32, %rdx
    orq  %rdx, %rax
    ret
.size kraos_rdtsc, .-kraos_rdtsc

/* ---- FS / GS 基址读写（arch_prctl 要用，必须特权指令）--------------------- */
.global kraos_set_fs
.type kraos_set_fs,@function
kraos_set_fs:
    movq %rdi, %rax
    movl $0xC0000100, %ecx            /* IA32_FS_BASE */
    wrmsr
    ret
.size kraos_set_fs, .-kraos_set_fs

.global kraos_get_fs
.type kraos_get_fs,@function
kraos_get_fs:
    movl $0xC0000100, %ecx
    rdmsr
    shlq $32, %rdx
    orq  %rdx, %rax
    ret
.size kraos_get_fs, .-kraos_get_fs

.global kraos_set_gs
.type kraos_set_gs,@function
kraos_set_gs:
    movq %rdi, %rax
    movl $0xC0000101, %ecx            /* IA32_GS_BASE */
    wrmsr
    ret
.size kraos_set_gs, .-kraos_set_gs

.global kraos_get_gs
.type kraos_get_gs,@function
kraos_get_gs:
    movl $0xC0000101, %ecx
    rdmsr
    shlq $32, %rdx
    orq  %rdx, %rax
    ret
.size kraos_get_gs, .-kraos_get_gs

/* 供 C 侧调用：输出一个以 0 结尾的字符串。 */
.global kraos_serial_puts
.type kraos_serial_puts,@function
kraos_serial_puts:
    movq %rdi, %rsi
    xorl %ecx, %ecx
9:
    cmpb $0, (%rsi,%rcx)
    je 10f
    incq %rcx
    jmp 9b
10:
    jmp  serial_puts_n
.size kraos_serial_puts, .-kraos_serial_puts

/* ---- 系统调用入口 --------------------------------------------------------
 *
 * syscall 指令进入时：rax = 调用号，rdi/rsi/rdx/r10/r8/r9 = 参数，
 * rcx = 返回地址，r11 = 返回标志，rsp 不变（64 位模式下 syscall 不压栈）。
 * 这里把参数铺到一个 C 调用约定能接受的栈帧上，转调 kraos_syscall（C 实现），
 * 再用 iretq 回到原处。
 */

/* rsi = 缓冲区，rcx = 字节数。逐字节送到 COM1，返回写出的字节数。 */
serial_puts_n:
    movq %rcx, %r8
    testq %r8, %r8
    jz 11f
12:
    movl $0x3fd, %edx                /* 线路状态寄存器 LSR */
13:
    inb %dx, %al
    testb $0x20, %al                 /* THRE：发送保持寄存器空 */
    jz 13b
    movl $0x3f8, %edx
    movzbl (%rsi), %eax
    outb %al, %dx
    incq %rsi
    decq %r8
    jnz 12b
11:
    movq %rcx, %rax
    subq %r8, %rax                   /* 返回实际写出的字节数 */
    ret

/* 供 C 侧调用：把一段二进制写到串口。 */
.global kraos_serial_write
.type kraos_serial_write,@function
kraos_serial_write:
    /* 形参是 (rdi=缓冲区, rsi=长度)，而 serial_puts_n 要的是 (rsi=缓冲区,
     * rcx=长度)。必须先搬长度再搬缓冲区 —— 踩过一次：第一条 mov 把长度顶掉
     * 之后 rcx 拿到的是缓冲区地址，于是按地址当长度把整片 .rodata 刷到了串口。 */
    movq %rsi, %rcx
    movq %rdi, %rsi
    jmp  serial_puts_n
.size kraos_serial_write, .-kraos_serial_write

.global syscall_entry
.type syscall_entry,@function
syscall_entry:
    /* 这一段是「以 syscall 指令的身份」被进来的：rax=调用号，rcx=返回地址，
     * r11=返回标志，rdi/rsi/rdx/r10/r8/r9=参数，rsp 指向调用者的返回地址。
     *
     * 坑在于 rbx 是被调用者保存寄存器。之前的写法是
     *     pushq %rbx / movq %rsp,%rbx 记下栈指针 / 返回时再从帧里读回来
     * 等于用栈地址覆盖了调用者的 rbx，而 syscall 的 ABI 并不要求调用者
     * 容忍这一点。后果非常隐蔽：initRuntime 里 new() 明明返回了合法堆指针，
     * 中间只隔着一次 gettid()（它尾跳到 syscall），rbx 就变成了栈地址，
     * 于是把栈上的垃圾当 ThreadData 写进去，最后在 AddRecord 里 #PF。
     *
     * 现在的分工：rbx 只在前两条指令里承载「原始 rsp - 8」，紧接着入帧；
     * 之后唯一被借用的临时是 r11 —— 按 syscall ABI r11 本来就可以被破坏，
     * 我们再从帧里把原值还回去。
     */
    pushq %rbx
    movq  %rsp, %rbx                 /* rbx = 原始 rsp - 8，(%rbx) 是刚压进去的调用者 rbx */
    subq  $160, %rsp
    andq  $-16, %rsp
    movq  %r11, 0x48(%rsp)           /* 返回标志先入帧，r11 从此当临时 */
    movq  (%rbx), %r11               /* 调用者的 rbx */
    movq  %r11, 0x68(%rsp)           /* 存入固定槽，返回前原样还回去 */
    movq  %rbx, 0x38(%rsp)           /* 原始 rsp - 8 */
    movq  %rax, 0x00(%rsp)           /* 调用号 */
    movq  %rdi, 0x08(%rsp)
    movq  %rsi, 0x10(%rsp)
    movq  %rdx, 0x18(%rsp)
    movq  %r10, 0x20(%rsp)
    movq  %r8,  0x28(%rsp)
    movq  %r9,  0x30(%rsp)
    movq  %rcx, 0x40(%rsp)           /* 返回地址 */

    /* C 调用的前 6 个参数走 rdi/rsi/rdx/rcx/r8/r9，第 7 个走栈。 */
    movq  0x00(%rsp), %rdi           /* nr */
    movq  0x08(%rsp), %rsi           /* a0 */
    movq  0x10(%rsp), %rdx           /* a1 */
    movq  0x18(%rsp), %rcx           /* a2 */
    movq  0x20(%rsp), %r8            /* a3 */
    movq  0x28(%rsp), %r9            /* a4 */
    movq  0x30(%rsp), %rax           /* a5 */
    /* call 会把返回地址压到 (%rsp)，所以第 7 参数要写在当前 (%rsp) 上，
       被调用方看到的才是它自己栈帧里的 8(%rsp)。 */
    movq  %rax, (%rsp)
    call  kraos_syscall               /* long kraos_syscall(long, long x6) */

    /* 返回用 iretq，不用 sysretq。
     *
     * sysretq 的 CS/SS 是从 STAR[63:48]+16/+8 推导的，要凑齐就得在 GDT 里
     * 另放一份 64 位代码段；一旦推导出来的选择子越界或类型不对，CPL 直接算错，
     * 表现是三重故障，排查成本远高于这里多搭 40 字节栈帧。内核本来也没有
     * 用户态/内核态之分，用 iretq 把 CS/SS/RFLAGS 全部显式写死最省事。
     */
    movq  %rax, %r10                  /* 系统调用返回值 */
    movq  0x40(%rsp), %rax            /* 返回地址 -> 新 RIP */
    movq  0x48(%rsp), %r11            /* 返回标志 */
    movq  0x38(%rsp), %rbx
    leaq  8(%rbx), %rbx               /* 原始 rsp */
    andq  $-513, %r11                 /* 保持 IF=0：P3 之前没有用户态 */

    movq  %rax, 0x40(%rsp)            /* iretq 帧：RIP */
    movq  $0x08, %rax
    movq  %rax, 0x48(%rsp)            /*         CS */
    movq  %r11, 0x50(%rsp)            /*         RFLAGS */
    movq  %rbx, 0x58(%rsp)            /*         RSP */
    movq  $0x10, %rax
    movq  %rax, 0x60(%rsp)            /*         SS */

    /* RSP 要指向帧首（RIP 所在），不是帧里装的那个值。 */
    leaq  0x40(%rsp), %rsp
    movq  0x28(%rsp), %rbx            /* ★ 把调用者的 rbx 还回去。
                                     *   rsp 此刻是 帧基+0x40，所以偏移要用 0x28
                                     *   才能落回帧里那个 0x68 的槽。 */
    movq  %r10, %rax
    iretq
.size syscall_entry, .-syscall_entry

/* ---- 中断描述符表 --------------------------------------------------------
 *
 * 没有 IDT 的裸机一旦出异常就是三重故障：CPU 连推两次栈都失败，于是复位，
 * 串口上什么也看不到，只能靠 QEMU 的 -d int 猜。装一张 IDT 之后，任何异常
 * 都会被统一入口截下，把「向量号 / 错误码 / 出错指令 / 出错地址」打到串口，
 * 再停机。这套打印 P3 的异常处理也要用。
 *
 * 64 位模式下 IDT 门里**不再有向量号字段**（向量 = 表项下标 * 16），所以每个
 * 向量各自有一个极小的桩，负责把自己的编号压栈，再跳到公共入口。
 */

.macro VECTOR_STUB n
    .globl vect_\n
vect_\n:
        pushq $\n
        jmp   fault_common
.endm

/* gas 的 x86 后端不会在 .rept 里替换 %，所以 0..31 这段显式列出。
 * 只有 32 行，且异常向量号本来就必须逐个准。 */
    VECTOR_STUB 0
    VECTOR_STUB 1
    VECTOR_STUB 2
    VECTOR_STUB 3
    VECTOR_STUB 4
    VECTOR_STUB 5
    VECTOR_STUB 6
    VECTOR_STUB 7
    VECTOR_STUB 8
    VECTOR_STUB 9
    VECTOR_STUB 10
    VECTOR_STUB 11
    VECTOR_STUB 12
    VECTOR_STUB 13
    VECTOR_STUB 14
    VECTOR_STUB 15
    VECTOR_STUB 16
    VECTOR_STUB 17
    VECTOR_STUB 18
    VECTOR_STUB 19
    VECTOR_STUB 20
    VECTOR_STUB 21
    VECTOR_STUB 22
    VECTOR_STUB 23
    VECTOR_STUB 24
    VECTOR_STUB 25
    VECTOR_STUB 26
    VECTOR_STUB 27
    VECTOR_STUB 28
    VECTOR_STUB 29
    VECTOR_STUB 30
    VECTOR_STUB 31

/* 中断向量 32..255 目前共用这个入口；P3 接上 PIC/APIC 后再按向量拆开。 */
.globl vect_unhandled
vect_unhandled:
    pushq $-1
    jmp   fault_common

/* 栈上（由低到高）：r15 r14 ... rax，向量号，[错误码]，rip，cs，rflags */
fault_common:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    cli
    movzbq 120(%rsp), %rdi              /* 向量号 */
    /* 只有一部分向量会额外压错误码。 */
    xorq  %r8, %r8
    cmpq  $8,  %rdi
    je  14f
    cmpq  $10, %rdi
    je  14f
    cmpq  $11, %rdi
    je  14f
    cmpq  $12, %rdi
    je  14f
    cmpq  $13, %rdi
    je  14f
    cmpq  $14, %rdi
    je  14f
    cmpq  $17, %rdi
    je  14f
    cmpq  $21, %rdi
    je  14f
    cmpq  $29, %rdi
    je  14f
    cmpq  $30, %rdi
    je  14f
    jmp  15f
14:
    movq  128(%rsp), %r8               /* 错误码 */
    movq  136(%rsp), %r9               /* rip */
    jmp  16f
15:
    movq  128(%rsp), %r9
16:
    movq  %r8, %rsi
    movq  %r9, %rdx
    movq  %rsp, %rcx                   /* 出错时的用户栈指针，仅作参考打印 */
    call  kraos_fault                  /* 不返回：打印完就停机 */
17:
    hlt
    jmp  17b

/* 装载 IDT。rdi 指向 10 字节的 IDT 描述符（2 字节界限 + 8 字节基址）。 */
.global kraos_lidt
.type kraos_lidt,@function
kraos_lidt:
    lidt (%rdi)
    ret
.size kraos_lidt, .-kraos_lidt

.global kraos_get_cr2
.type kraos_get_cr2,@function
kraos_get_cr2:
    movq %cr2, %rax
    ret
.size kraos_get_cr2, .-kraos_get_cr2

/* ------------------------------------------------------------------ *
 *  低半区只读数据：GDT + 32 位启动阶段的提示语                          *
 * ------------------------------------------------------------------ */
.section .rodata.boot,"a"
.balign 8
/* 4 项，够 syscall 与 sysretq 同时取用：
 *   0x00 空
 *   0x08 64 位代码段（syscall 进入时的 CS，也是内核主 CS）
 *   0x10 数据段（syscall 的 SS = CS+8，也是 sysretq 的 SS）
 *   0x18 64 位代码段（sysretq 的 CS = STAR[63:48]+16）
 * 为什么 0x08 与 0x18 要两份 64 位代码段：syscall 用 STAR[47:32] 选 CS，
 * sysretq 用 STAR[63:48]+16 选 CS，而 sysretq 的 SS 又是 STAR[63:48]+8，
 * 两边的推导规则不同，共用一项会让其中一个落到越界或类型不对的选择子上。
 *
 * GDT 必须留在低半区：gdt_pointer 的基址字段是 .long（32 位），
 * 放到 0xffffffff80000000 会被截断成 0x00000000。
 */

gdt:
    .quad 0
    .quad 0x00af9a000000ffff       /* 64 位代码段，ring 0 */
    .quad 0x00cf92000000ffff       /* 数据段，ring 0 */
    .quad 0x00af9a000000ffff       /* 64 位代码段，ring 0 */
gdt_end:
gdt_pointer:
    .word gdt_end-gdt-1
    .long gdt
msg_entry:
    .asciz "[boot] Multiboot2 入口，正在准备 x86_64 长模式\r\n"
msg_lm_ok:    .asciz "[boot] long mode 可用\r\n"
msg_paging:   .asciz "[boot] 已开启 PAE/LME/PG\r\n"
msg_cpu_error:
    .asciz "[boot] FATAL: 需要 x86_64 long mode\r\n"

/* ------------------------------------------------------------------ *
 *  高半区只读数据：长模式与 Kotlin 移交阶段的提示语                      *
 * ------------------------------------------------------------------ */
.section .rodata,"a"
.balign 8
msg_lm:       .asciz "[boot] 已进入 64 位长模式 @ 0xffffffff80000000\r\n"
msg_konan:    .asciz "[boot] 移交 Konan_main\r\n"
msg_konan_done: .asciz "[boot] Konan_main 已返回\r\n"
msg_halt:
    .asciz "[boot] 内核 main 返回，停机\r\n"

/* ------------------------------------------------------------------ *
 *  低半区数据与 BSS                                                     *
 * ------------------------------------------------------------------ */

/* Multiboot2 头与信息指针：_start 在 32 位下用绝对寻址写这两个字，
   因此必须待在 4 GiB 以内。 */
.section .data.boot,"aw"
.balign 4
mb_magic:  .skip 4
mb_info:   .skip 8

/* 引导桩自己的东西全部留在低半区：页表、引导栈、TLS 块、静态堆。
   它们在切到高半区之前就要用，而那时候高半区的映射还不存在。 */
/* __bss_boot_start/__bss_boot_end 由链接脚本定义，不要在这里重复打标签 ——
   .bss.boot 的内容里 kraos_heap 要求 2 MiB 对齐，输出段起点会被顶到 2 MiB 边界，
   脚本在放置输入段**之前**取的 `.` 与打完标签后的实际位置并不相同。 */
.section .bss.boot,"aw",@nobits
.balign 4096
kraos_tls_block:
    .skip KRAOS_TLS_SIZE
.balign 4096
pml4:            .skip 4096
pdpt:            .skip 4096
page_directories:.skip 16384           /* 4 张，线性映射 [0, 4 GiB) */
hi_pdpt:         .skip 4096            /* 高半区页目录指针表 */
hi_pd:           .skip 4096            /* 高半区页目录 */
.balign 16
boot_stack_bottom: .skip 262144
boot_stack_top:

/* 引导期静态堆。P2 之后运行时主堆走 multiboot2 内存图的位图分配器，
   这块仍留给引导期与 malloc 过渡使用。 */
.balign 2097152
.global kraos_heap
kraos_heap:
    .skip 67108864                 /* 64 MiB */
kraos_heap_end:

.section .note.GNU-stack,"",@progbits
