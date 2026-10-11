/*
 * libc 内存分配入口（C ABI）。
 *
 * 为什么这一层必须是 C 而不是 Kotlin：
 *
 * Kotlin/Native 的 @CName 导出只有在「K/N 运行时位码引用了这个符号」时才
 * 会真的生成对外可见的全局符号。运行时的位码引用了 malloc/calloc/free/
 * write/abort，所以 Kotlin 侧的同名实现能生效；但 glibc 静态库里 __brk、
 * __mmap、__sbrk 这些是 **libc.a 内部成员之间**的调用，K/N 编译时看不见，
 * 于是 Kotlin 侧导出的 brk/sbrk/mmap 要么根本没被生成，要么被 -opt 内联掉，
 * 最后链接期只剩一句 undefined symbol。
 *
 * gcc 编的 C 函数则是实打实的 .globl 强符号，没有这个不确定性。
 *
 * 为什么还需要 --wrap：
 * glibc 自己的 brk 是 `mov $0xc,%eax; syscall`，裸机上执行 syscall 指令
 * 直接触发 #UD。Makefile 里用 -linker-option=--wrap=__brk 把 libc.a 内部
 * 所有对 __brk 的调用改道到本文件的 __wrap___brk。
 *
 * 内存布局：整个 arena 由引导桩在 .bss 里划出 64 MiB。这里切成互不相干
 * 的两半 ——
 *   下半（32 MiB）  给 glibc malloc，通过 brk/sbrk 往前长；
 *   上半（32 MiB）  给 Kotlin/K/N 的 mmap 路径，通过 kraos_alloc 往后长。
 * 分开是为了避免两套分配器各自推进游标时互相踩踏。
 */

#include <stddef.h>
#include <cpuid.h>

/* freestanding 下没有系统头，直接把 mmap 最后一个参数的宽度写死。 */
typedef long kraos_off_t;

/* 引导桩提供的边界与特权指令入口（boot/entry.S）。 */
extern unsigned long kraos_arena_start(void);
extern unsigned long kraos_arena_end(void);

/* 静态堆切成两半时的分界线（占下半 32 MiB 给 glibc malloc）。 */
#define KRAOS_BRK_SHARE 32UL * 1024UL * 1024UL

#define KRAOS_ALIGN 16UL

static unsigned long g_brk_cursor;   /* glibc malloc 的断点 */
static unsigned long g_brk_limit;
static unsigned long g_alloc_cursor; /* Kotlin / K/N 的 mmap 游标 */
static unsigned long g_alloc_limit;
static unsigned long g_total_bytes;
static unsigned long g_alloc_count;

static void heap_init(void)
{
    unsigned long start, end;

    if (g_brk_limit != 0) {
        return; /* 已经初始化过 */
    }
    start = (kraos_arena_start() + (KRAOS_ALIGN - 1)) & ~(KRAOS_ALIGN - 1);
    end = kraos_arena_end();
    if (end <= start) {
        /* 边界非法时让两半都空着，后续分配会直接失败 */
        g_brk_limit = start;
        g_alloc_cursor = end;
        g_alloc_limit = end;
        return;
    }

    g_brk_cursor = start;
    g_brk_limit = start + (end - start) / 2;  /* 下半：给 malloc */
    g_alloc_cursor = (g_brk_limit + (KRAOS_ALIGN - 1)) & ~(KRAOS_ALIGN - 1);
    g_alloc_limit = end;                      /* 上半：给 mmap 路径 */
}

/* 供 Kotlin 侧 BootHeap 使用的分配入口。 */
unsigned long kraos_alloc(unsigned long size)
{
    unsigned long need, base;

    heap_init();
    if (size == 0) {
        size = KRAOS_ALIGN;
    }
    need = (size + (KRAOS_ALIGN - 1)) & ~(KRAOS_ALIGN - 1);
    if (need < size) {
        return 0; /* 溢出 */
    }

    base = g_alloc_cursor;
    if (base + need < base || base + need > g_alloc_limit) {
        return 0; /* 上半用尽 */
    }
    g_alloc_cursor = base + need;
    g_total_bytes += need;
    g_alloc_count++;

    /* .bss 不保证是零，Kotlin 对象头与 GC 元数据都依赖清零。 */
    {
        unsigned long *p = (unsigned long *)base;
        unsigned long n = need / sizeof(unsigned long);
        while (n--) {
            *p++ = 0;
        }
    }
    return base;
}

unsigned long kraos_heap_start(void) { heap_init(); return g_brk_cursor; }
unsigned long kraos_heap_end(void)   { heap_init(); return g_alloc_limit; }
unsigned long kraos_alloc_bytes(void){ return g_total_bytes; }
unsigned long kraos_alloc_count(void){ return g_alloc_count; }

/* ---- brk / sbrk 的真实实现 ------------------------------------------------ */

unsigned long kraos_brk_impl(unsigned long addr)
{
    unsigned long start, end;

    heap_init();
    start = g_brk_cursor;
    end = g_brk_limit;
    if (addr >= start && addr <= end) {
        g_brk_cursor = addr;
        return addr;
    }
    return g_brk_cursor;
}


/*
 * ---- 系统调用分派 --------------------------------------------------------
 *
 * K/N 运行时借用的 glibc 静态库里有一批包装是直接下 syscall 指令的，不经过
 * 任何可链接的符号，所以链接期的 --wrap 够不着它们。entry.S 把 IA32_LSTAR 指
 * 到 syscall_entry（它把参数铺成 C 调用约定），真正的实现在这里。
 *
 * 这里是 P1 的最小可用集合：能让 glibc 完成它的启动探测即可。P3 有了调度器
 * 之后再把 futex / clock 换成真正的实现。
 */

#define SYS_read           0
#define SYS_write          1
#define SYS_mmap           9
#define SYS_mprotect       10
#define SYS_munmap         11
#define SYS_brk            12
#define SYS_sched_yield   24
#define SYS_getpid        39
#define SYS_arch_prctl   158
#define SYS_gettid       186
#define SYS_futex        202
#define SYS_clock_gettime 228
#define SYS_getrandom    318

#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_GS 0x1004
#define ARCH_GET_FS 0x1003

#define EPERM    1
#define ENOSYS  38
#define EBADF    9

/* 单次 write 的上限，防止参数出错时把串口刷爆。 */
#define KRAOS_WRITE_MAX (64 * 1024)
#define EAGAIN  11

/* 引导桩提供的特权接口。 */
extern void  kraos_set_fs(void *base);
extern void *kraos_get_fs(void);
extern void  kraos_set_gs(void *base);
extern void *kraos_get_gs(void);
extern unsigned long kraos_rdtsc(void);
extern long  kraos_serial_write(const void *buf, unsigned long len);

/* 异常处理入口。向量号 -1 表示「还没拆开的硬件中断」。 */
extern unsigned long kraos_get_cr2(void);
extern void kraos_cli(void);
extern void kraos_hlt(void);

static void put_hex(unsigned long v)
{
    static const char digits[] = "0123456789abcdef";
    char buf[16];
    int i;

    for (i = 15; i >= 0; i--) { buf[i] = digits[v & 0xf]; v >>= 4; }
    kraos_serial_write(buf, 16);
}

static void put_dec(unsigned long v)
{
    char buf[24];
    int n = 0, i;

    if (v == 0) { kraos_serial_write("0", 1); return; }
    while (v) { buf[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (i = 0; i < n; i++) kraos_serial_write(&buf[n - 1 - i], 1);
}

static void put_str(const char *s) { kraos_serial_write(s, __builtin_strlen(s)); }

/* 打印完就停机：裸机上没有别的进程可退，halt 比空转友好，也便于 QEMU 抓现场。
 *
 * rsp 指向 fault_common 保存的寄存器区（从低到高是 rax rbx rcx rdx rsi rdi
 * rbp r8..r15），一并打出来 —— 内核里「指针长得像两个 32 位整数拼起来的数」
 * 这种问题，光看 RIP 和 CR2 是猜不出来的。 */
void kraos_fault(unsigned long vector, unsigned long errcode, unsigned long rip,
                unsigned long saved_rsp)
{
    const unsigned long *r = (const unsigned long *)saved_rsp;
    static const char names[][4] = {
        "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "r8 ",
        "r9 ", "r10", "r11", "r12", "r13", "r14", "r15",
    };
    int i;

    put_str("[kraos] 异常 vector=");
    if (vector == (unsigned long)-1) put_str("IRQ(未拆分)");
    else put_dec(vector);
    put_str(" errcode=");
    put_hex(errcode);
    put_str(" rip=");
    put_hex(rip);
    put_str(" cr2=");
    put_hex(kraos_get_cr2());
    put_str(" rsp=");
    put_hex(saved_rsp);
    kraos_serial_write("\r\n", 2);

    for (i = 14; i >= 0; i--) {
        put_str("      ");
        put_str(names[i]);
        put_str("=");
        put_hex(r[i]);
        kraos_serial_write("\r\n", 2);
    }

    /* K/N 的 __thread 变量布局在 TP(=*%fs:0) 之下的负偏移处：
     *   fs:0    -> struct pthread（glibc TCB，自指针）
     *   fs:-0x48 -> kotlin::mm::ThreadRegistry::currentThreadDataNode_
     *   fs:-0x60 -> 另一个被 initRuntime 惰性初始化的线程局部
     * 负偏移怎么来的：链接期符号值是「在 TLS 段内的偏移」（currentThreadDataNode_
     * 是 0x48），而 PT_TLS 的 memsz 是 0x90，所以实际 fs 位移 = 0x48 - 0x90 = -0x48。
     * 这三个值能一眼看出「当前线程到底登记上没有」。 */
    {
        unsigned long tp, lazy;
        __asm__ volatile ("movq %%fs:0, %0" : "=r"(tp));
        lazy = *(volatile unsigned long *)(tp - 0x60);
        put_str("[kraos] TLS tp=");
        put_hex(tp);
        put_str(" node(fs:-0x48)=");
        put_hex(*(volatile unsigned long *)(tp - 0x48));
        put_str(" lazy(fs:-0x60)=");
        put_hex(lazy);
        put_str(" lazy[0]=");
        /* 异常处理器里绝不能再解引用一个可能为 0 的指针：真发生了就是三重故障，
         * 连上面这些诊断信息都来不及打印。所以只读不解。 */
        if (lazy != 0 && lazy < 0x100000000UL)
            put_hex(*(volatile unsigned long *)lazy);
        else
            put_str("(不读)");
        kraos_serial_write("\r\n", 2);
    }

    /* 把出错现场指向的那块内存倒出来。内核里最常见的一类问题就是「某个结构体
     * 里的字段是脏数据」—— 光看 RIP 和 CR2 是猜不出来的。 */
    put_str("[kraos] 内存 rbx:");
    kraos_serial_write("\r\n", 2);
    for (i = 0; i < 8; i++) {
        put_str("      +");
        put_hex((unsigned long)(i * 8));
        put_str(": ");
        put_hex(((const unsigned long *)r[1])[i]);
        kraos_serial_write("\r\n", 2);
    }

    kraos_cli();
    for (;;) kraos_hlt();
}

/* 只有一个线程，所有调用者看到的都是 1。 */
static long g_next_tid = 1;

/* ---- futex ----
 *
 * 单核单线程下 futex 只有两种「诚实」的答案：
 *   FUTEX_WAKE  —— 一个等待者都没有，返回 0；
 *   FUTEX_WAIT  —— 调用方在「*uaddr 还是 val」时才要求睡。单线程里没人能
 *                  唤醒它，真睡下去就是死锁；此时要么 *uaddr 已经变了（正常
 *                  竞争失败，返回 EAGAIN 让 glibc 重新检查条件），要么就是
 *                  真的死锁了 —— 明确报出来并停机，好过无声空转。
 *
 * 之前对所有操作一律返回 -EAGAIN，WAKE 拿到负数会把 glibc 的条件变量逻辑
 * 彻底带偏：pthread_cond_wait 把 EAGAIN 当伪唤醒反复重试，于是看到
 * __pthread_mutex_lock 被连续调用几十次而状态位始终不前进。
 */
#define FUTEX_CMD_MASK      0x7f
#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_REQUEUE       3
#define FUTEX_CMP_REQUEUE   4
#define FUTEX_WAKE_OP       5
#define FUTEX_WAIT_BITSET   9
#define FUTEX_WAKE_BITSET  10

/* 真的需要睡时，轮这么久 TSC 仍然没人来唤醒，就判定为死锁并报告。 */
#define FUTEX_STALL_TICKS   2000000000ULL

static void kraos_futex_stall(unsigned long uaddr, unsigned int want, unsigned int got)
{
    unsigned long long t0 = kraos_rdtsc();

    for (;;) {
        volatile unsigned int *p = (volatile unsigned int *)uaddr;
        if (*p != want) return;              /* 有人改了，交给调用方重试 */
        if (kraos_rdtsc() - t0 > FUTEX_STALL_TICKS) {
            put_str("[kraos] futex 死锁: 等待 ");
            put_hex(uaddr);
            put_str(" 由 ");
            put_hex(want);
            put_str(" 变成别的值，当前仍是 ");
            put_hex(got);
            kraos_serial_write("\r\n", 2);
            kraos_cli();
            for (;;) kraos_hlt();
        }
        __asm__ volatile ("hlt");
    }
}


/* ---- CPU 拓扑 ----
 *
 * 核数优先走 CPUID 叶 0xB（扩展拓扑枚举）：subleaf 0 的 EBX[15:0] 就是逻辑
 * 处理器总数，一步到位。
 *
 * 注意别只读叶 4 的 subleaf 0 —— 那个 EBX[31:24] 是「当前缓存拓扑层级下的
 * 线程数」，单核无 SMT 时恒为 1，多核机器上照样报 1。必须逐个 subleaf 累加。
 */
long kraos_cpu_count(void)
{
    unsigned int a, b, c, d, sub;
    unsigned int max = __get_cpuid_max(0, 0);
    long n;

    if (max >= 0xB) {
        /*
         * 叶 0xB 逐 subleaf 描述拓扑层级：0 号通常是 SMT（「每核几个线程」），
         * 后面才是核心、包。总核数要取**最后一个有效层级**的 ebx[15:0] ——
         * 只看 subleaf 0 的话，无 SMT 的机器恒为 1（这正是「-smp 4 报 1 核」）。
         *
         * 终止条件踩过坑：规范说 ECX[15:0]（level type）为 0 即无效，但 QEMU
         * 对越界的 subleaf 把 ECX 原样填成 subleaf 序号（sub=2 回来 2、
         * sub=3 回来 3……），永远非零，循环会一路跑满、把 n 覆盖成 0。
         * EBX 才是稳的：有效层级的「逻辑处理器数」必非零，无效层级必为 0。
         */
        n = 0;
        for (sub = 0; sub < 16; sub++) {
            __cpuid_count(0xB, sub, a, b, c, d);
            if (b == 0) break;
            n = (long)(b & 0xFFFF);
        }
        if (n > 0) return n;
    }

    if (max >= 4) {
        /* 退回叶 4：逐 subleaf 累加 EBX[31:24]。 */
        n = 0;
        for (sub = 0; ; sub++) {
            __cpuid_count(4, sub, a, b, c, d);
            if ((b & 0x1F) == 0) break;      /* cache type 0 = 无效项 */
            n += (long)((b >> 24) & 0xFF);
        }
        if (n > 0) return n;
    }

    if (max >= 1) {
        __cpuid_count(1, 0, a, b, c, d);
        return (long)(((b >> 24) & 0xFF) + 1);   /* 初始 APIC ID + 1 */
    }
    return 1;
}

long kraos_syscall(long nr, long a0, long a1, long a2, long a3, long a4, long a5)
{
    (void)a4; (void)a5;

    switch (nr) {
    case SYS_write:
        /* 只把标准输出/错误接到串口，其余 fd 一律 EBADF。 */
        if (a0 == 1 || a0 == 2) {
            long n = a2;
            if (n <= 0) return 0;
            /* 单次写太多会把串口刷爆（一次 5 万字节的误传就够刷 50 MB），
             * 超过上限就截断，并把真实长度如实返回给调用方。 */
            if (n > KRAOS_WRITE_MAX) n = KRAOS_WRITE_MAX;
            return (long)kraos_serial_write((const void *)a1, (unsigned long)n);
        }
        return -EBADF;

    case SYS_read:
        return -EBADF;               /* 没有输入设备 */

    case SYS_brk:
        return (long)kraos_brk_impl((unsigned long)a0);

    case SYS_mmap: {
        /* 只支持匿名映射；文件映射尚无文件系统语义。 */
        if (a4 > 0) return -1;
        unsigned long p = kraos_alloc((unsigned long)a1);
        return p ? (long)p : -1;
    }

    case SYS_munmap:
        (void)a0; (void)a1;
        return 0; /* 引导阶段 bump arena 不回收 */

    case SYS_mprotect:
        (void)a0; (void)a1; (void)a2;
        return 0; /* 当前没有用户态映射保护语义 */

    case SYS_arch_prctl:
        switch (a0) {
        case ARCH_SET_FS: kraos_set_fs((void *)a1); return 0;
        case ARCH_SET_GS: kraos_set_gs((void *)a1); return 0;
        case ARCH_GET_FS: *(unsigned long *)a1 = (unsigned long)kraos_get_fs(); return 0;
        case ARCH_GET_GS: *(unsigned long *)a1 = (unsigned long)kraos_get_gs(); return 0;
        default: return -ENOSYS;
        }

    case SYS_getpid:
    case SYS_gettid:
        return g_next_tid;

    case SYS_sched_yield:
        return 0;                    /* 单核单线程，没什么可让的 */

    case SYS_futex: {
        unsigned int op   = (unsigned int)a1;
        unsigned int cmd  = op & FUTEX_CMD_MASK;   /* 去掉 PRIVATE / CLOCK_REALTIME */
        volatile unsigned int *uaddr = (volatile unsigned int *)a0;
        unsigned int val = (unsigned int)a2;

        switch (cmd) {
        case FUTEX_WAIT:
        case FUTEX_WAIT_BITSET:
            if (*uaddr != val) return -EAGAIN;     /* 竞争失败：语义正确 */
            kraos_futex_stall((unsigned long)uaddr, val, *uaddr);
            return -EAGAIN;

        case FUTEX_WAKE:
        case FUTEX_WAKE_BITSET:
            return 0;                              /* 单线程：没有等待者 */

        case FUTEX_REQUEUE:
        case FUTEX_CMP_REQUEUE:
        case FUTEX_WAKE_OP:
            return 0;

        default:
            return -ENOSYS;                        /* PI / LOCK_PI 等未实现 */
        }
    }

    case SYS_clock_gettime: {
        /* 用 TSC 造一个单调递增的秒/纳秒，频率按 1 GHz 估。
         * P3 换成读 ACPI/HPET 的真实时间源。 */
        struct timespec_k { long tv_sec; long tv_nsec; } ts;
        unsigned long long ticks = kraos_rdtsc();
        ts.tv_sec = (long)(ticks / 1000000000ULL);
        ts.tv_nsec = (long)((ticks % 1000000000ULL) * 1000ULL);
        if (a1 != 0) *(struct timespec_k *)a1 = ts;
        return 0;
    }

    case SYS_getrandom: {
        /* 给一个确定性的伪随机流，够 glibc 启动探测用。 */
        unsigned char *p = (unsigned char *)a0;
        long n = a1, i;
        unsigned long long s = 0x9E3779B97F4A7C15ULL;
        for (i = 0; i < n; i++) {
            s ^= s << 13; s ^= s >> 7; s ^= s << 17;
            p[i] = (unsigned char)(s >> 24);
        }
        return n;
    }

    default:
        break;
    }
    return -ENOSYS;
}

/*
 * ---- IRELATIVE（ifunc）重定位 --------------------------------------------
 *
 * lld 链接 glibc 静态库时，会给它里面的 ifunc（memcpy、strlen、memmove 这类
 * 多版本实现）生成 .iplt 桩，桩体是一条「jmp *GOT 槽」。lld 不在链接期执行解析
 * 器，而是把解析器地址写进 .rela.iplt 里的 IRELATIVE 重定位，留给运行时填。
 * 正常的 glibc 静态程序由 LIBC_START_MAIN 调 __libc_csu_irel 干这件事；我们是
 * 直接跳进 Konan_main 的，没走那条路，所以自己补上。
 */

struct kraos_rela {
    unsigned long long r_offset;
    unsigned long long r_info;
    long long r_addend;
};

/* 弱引用：万一链接进来的 libc 里没有 libc-start.o，这里就是 NULL，直接跳过。 */
extern const struct kraos_rela __rela_iplt_start[] __attribute__((weak));
extern const struct kraos_rela __rela_iplt_end[] __attribute__((weak));

#define R_X86_64_IRELATIVE 37

int kraos_apply_irel(void)
{
    const struct kraos_rela *r;
    int n = 0;

    if (__rela_iplt_start == 0 || __rela_iplt_end == 0) {
        return 0;
    }
    for (r = __rela_iplt_start; r < __rela_iplt_end; ++r) {
        if ((r->r_info & 0xffffffffu) != R_X86_64_IRELATIVE) continue;
        /* 解析器地址放在 r_addend 里，返回值就是要写进 GOT 的实现地址。 */
        *(void **)(unsigned long)r->r_offset =
            ((void *(*)(void))(unsigned long)r->r_addend)();
        n++;
    }
    return n;
}

/*
 * ---- 中断描述符表 --------------------------------------------------------
 *
 * 裸机没有 IDT 时，任何异常都是三重故障：CPU 连推两次栈都失败，直接复位，
 * 串口上什么都看不到，只能靠 QEMU 的 -d int 反推。装上 IDT 之后，异常会被
 * entry.S 的 fault_common 截住，打印向量号/错误码/出错指令/出错地址再停机。
 * P3 的异常处理也要用这套。
 *
 * 表在 C 里建而不是写在汇编里：gas 的 x86 后端不允许对「节内符号」做移位，
 * 而 16 字节门描述符里的偏移要拆成 16/16/32 三段。用 C 组装就没有这个限制。
 */

/* 64 位 IDT 门：偏移低 16 位、选择子、IST、类型属性、偏移中 16 位、偏移高 32 位。 */
struct kraos_idt_gate {
    unsigned short off_low;
    unsigned short selector;
    unsigned char  ist;         /* 中断栈表索引，这里恒为 0 */
    unsigned char  attr;        /* P/DPL/S/Type：0x8E = 64 位中断门 */
    unsigned short off_middle;
    unsigned int   off_high;
    unsigned int   reserved;
} __attribute__((packed));

struct kraos_idtr {
    unsigned short limit;
    unsigned long  base;
} __attribute__((packed));

extern void fault_common(void);
extern void vect_unhandled(void);

/* entry.S 里为 0..31 号异常各生成的一个只压向量号的小桩。 */
#define V(n) extern char vect_##n[];
V(0)  V(1)  V(2)  V(3)  V(4)  V(5)  V(6)  V(7)
V(8)  V(9)  V(10) V(11) V(12) V(13) V(14) V(15)
V(16) V(17) V(18) V(19) V(20) V(21) V(22) V(23)
V(24) V(25) V(26) V(27) V(28) V(29) V(30) V(31)

static struct kraos_idt_gate g_idt[256] __attribute__((aligned(16)));

static void set_gate(struct kraos_idt_gate *g, unsigned long handler)
{
    g->off_low   = (unsigned short)(handler & 0xffff);
    g->selector  = 0x08;                 /* 内核 64 位代码段 */
    g->ist       = 0x00;                 /* 不用独立的内核栈 */
    g->attr      = 0x8e;                 /* P=1 DPL=0 S=0 Type=0b01110 */
    g->off_middle= (unsigned short)((handler >> 16) & 0xffff);
    g->off_high  = (unsigned int)(handler >> 32);
    g->reserved  = 0;
}

extern void kraos_lidt(const struct kraos_idtr *);

void kraos_setup_idt(void)
{
    static const unsigned long stubs[32] = {
        (unsigned long)vect_0,  (unsigned long)vect_1,  (unsigned long)vect_2,
        (unsigned long)vect_3,  (unsigned long)vect_4,  (unsigned long)vect_5,
        (unsigned long)vect_6,  (unsigned long)vect_7,  (unsigned long)vect_8,
        (unsigned long)vect_9,  (unsigned long)vect_10, (unsigned long)vect_11,
        (unsigned long)vect_12, (unsigned long)vect_13, (unsigned long)vect_14,
        (unsigned long)vect_15, (unsigned long)vect_16, (unsigned long)vect_17,
        (unsigned long)vect_18, (unsigned long)vect_19, (unsigned long)vect_20,
        (unsigned long)vect_21, (unsigned long)vect_22, (unsigned long)vect_23,
        (unsigned long)vect_24, (unsigned long)vect_25, (unsigned long)vect_26,
        (unsigned long)vect_27, (unsigned long)vect_28, (unsigned long)vect_29,
        (unsigned long)vect_30, (unsigned long)vect_31,
    };
    struct kraos_idtr dtr;
    int i;

    for (i = 0; i < 32; i++) set_gate(&g_idt[i], stubs[i]);
    for (; i < 256; i++)      set_gate(&g_idt[i], (unsigned long)vect_unhandled);

    dtr.limit = sizeof(g_idt) - 1;
    dtr.base  = (unsigned long)g_idt;
    kraos_lidt(&dtr);
}
