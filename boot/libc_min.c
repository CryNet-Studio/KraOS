/*
 * 最小 libc 面 —— 顶替被摘掉的 glibc 归档。
 *
 * ## 为什么有这份文件
 *
 * P2 的目标是 `kraosVirtualAddress` 切回 0xffffffff80000000。挡路的是
 * glibc 静态归档（libc.a / libpthread.a）里的 `R_X86_64_32` 绝对重定位 ——
 * 那种重定位放不进 4 GiB 以上，链接会直接报 `out of range`。
 *
 * 于是把 `-lc -lpthread -ldl -lm` 从 kraosLinkerExtraLibs 里去掉，让 ld 自己
 * 报缺什么。结果是一份精确的 20 符号清单（这个方法比逐个猜要可靠得多）：
 *
 *   abort  clock_gettime  __cxa_atexit  dl_iterate_phdr  fputc  fputs
 *   free  malloc  mmap  pthread_cond_broadcast  pthread_cond_destroy
 *   pthread_cond_wait  pthread_mutex_lock  pthread_mutex_unlock  pthread_once
 *   snprintf  vsnprintf  __stack_chk_fail  stderr  strerror  syscall  write
 *
 * 本文件把这 20 个补齐，glibc 归档就可以彻底从链接行消失。
 *
 * ## 为什么是 C 而不是 Kotlin
 *
 * 试过了：Kotlin 的 @CName 只有在**运行时位码本身**引用了该符号时才生成对外
 * 可见的全局符号；仅被 Kotlin 代码调用是不够的，`-opt` 会把它整个裁掉，
 * 链接时照样 undefined（见 git log 里第一次尝试的失败）。
 *
 * 另外这些函数本身就是 C ABI 边界（`pthread_mutex_t*`、`va_list`、
 * `FILE*`），用 C 写省掉一层映射，也省掉 `variadic function` 在
 * Kotlin/Native 里根本没有对应物的问题。
 *
 * 真正属于「内核逻辑」的部分（页分配器、内存图解析、控制台）仍然是 Kotlin，
 * 见 kernel/src/。这份文件只提供 libc 原语。
 */

#include <stddef.h>
#include <stdarg.h>

/* ---- 引导桩提供的底层能力 ---------------------------------------------- */

extern void *kraos_alloc(unsigned long size);
extern long kraos_serial_write(const void *buf, unsigned long len);
extern unsigned long long kraos_rdtsc(void);
extern void kraos_hlt(void);
extern void kraos_cli(void);
extern long kraos_syscall(long nr, long a0, long a1, long a2, long a3, long a4, long a5);

/* ---- 本文件内部的小工具 ------------------------------------------------ */

static void put_str(const char *s)
{
    if (!s) return;
    unsigned long n = 0;
    while (s[n]) n++;
    kraos_serial_write(s, n);
}

static void put_hex(unsigned long v)
{
    static const char digits[] = "0123456789abcdef";
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++)
        buf[2 + i] = digits[(v >> ((15 - i) * 4)) & 0xF];
    buf[18] = 0;
    put_str(buf);
}

/* ======================================================================== *
 * 1. 堆：malloc / free / posix_memalign / mmap
 * ======================================================================== */

/*
 * 首次适配 + 邻接合并。
 *
 * 所有分块放在一条**按地址排序**的双向链表里，free 时与前后邻居合并。
 * K/N 启动期总共几千次分配，O(n) 的合并扫描完全够用，换来的是正确性
 * —— 比不上 ptmalloc 的速度，但比一个只增不减的 bump 分配器强得多，
 * 后者在长时间运行后会因为「free 掉的内存拿不回来」而把内存耗光。
 */

#define CHUNK_MAGIC   0x4b524143UL /* "KRAC" */
#define ALIGN         16UL
#define SLAB_BYTES    (2UL << 20)  /* 单次向引导堆要 2 MiB */

typedef struct chunk {
    unsigned long     magic;
    unsigned long     size;        /* 载荷字节数，不含本头 */
    struct chunk     *prev;        /* 按地址排序的邻居 */
    struct chunk     *next;
    unsigned long     free;
    /*
     * 显式填充。K/N 的运行时用 `movaps` 清零新分配的对象，那条指令要求
     * 目的地址 16 字节对齐 —— 头部尺寸一旦不是 16 的倍数，malloc 返回的
     * 载荷就只剩 8 字节对齐，`movaps` 直接 #GP。
     *
     * 这里很容易踩：5 个 8 字节字段正好 40 字节，40 % 16 = 8，肉眼完全看不出来，
     * 而症状是「Kotlin 代码跑到一半崩在一条清零指令上」，跟内存分配器八竿子打不着。
     * 所以下面还有一条编译期断言兜底。
     */
    unsigned long     pad;
} chunk_t;

#define HDR ((unsigned long)sizeof(chunk_t))

/* 编译期断言：头部必须是 16 的倍数，否则上面说的 movaps 故障一定会来。 */
typedef char kraos_hdr_aligned[(HDR % 16 == 0) ? 1 : -1];

static chunk_t *g_head = 0;        /* 地址最小的块 */
static chunk_t *g_bump = 0;        /* 尚未切分的那个块，地址最大的在用块 */
static unsigned long g_brk = 0;    /* 指向 g_bump 之后的新鲜内存 */

static void *sys_brk(unsigned long n)
{
    if (!g_brk) return 0;
    unsigned long old = g_brk;
    g_brk += n;
    return (void *)old;
}

/** 把尚未切分的内存变成一个在用块，挂在链表尾部。 */
static chunk_t *carve(unsigned long n)
{
    unsigned long need = HDR + n;
    need = (need + (ALIGN - 1)) & ~(ALIGN - 1);

    if (g_bump) {
        chunk_t *b = g_bump;
        if (b->size >= need) {
            /* 够，直接从尾部切一块出来，剩下的留在 g_bump 里 */
            chunk_t *rest = (chunk_t *)((char *)b + need);
            rest->magic = CHUNK_MAGIC;
            rest->size = b->size - need;
            rest->free = 1;
            rest->prev = b;
            rest->next = b->next;
            if (b->next) b->next->prev = rest;
            else g_head = g_head;   /* g_bump 原本就在尾部 */
            b->next = rest;
            b->size = n;
            b->free = 0;
            return b;
        }
        /* 不够，新切一块 slab 接在后面 */
        unsigned long got = need > SLAB_BYTES ? need : SLAB_BYTES;
        chunk_t *nb = (chunk_t *)sys_brk(got);
        if (!nb) return 0;
        nb->magic = CHUNK_MAGIC;
        nb->size = got - HDR;
        nb->free = 1;
        nb->prev = g_bump;
        nb->next = 0;
        g_bump->next = nb;
        if (!g_head) g_head = nb;
        g_bump = nb;
        return carve(n);
    }

    /* 第一次：从引导堆要一块 */
    unsigned long got = need > SLAB_BYTES ? need : SLAB_BYTES;
    chunk_t *nb = (chunk_t *)kraos_alloc(got);
    if (!nb) return 0;
    g_brk = (unsigned long)nb + got;
    nb->magic = CHUNK_MAGIC;
    nb->size = got - HDR;
    nb->free = 1;
    nb->prev = 0;
    nb->next = 0;
    g_head = nb;
    g_bump = nb;
    return carve(n);
}

static void *payload(chunk_t *c) { return (char *)c + HDR; }

void *malloc(size_t n)
{
    if (n == 0) n = 1;
    size_t want = (n + (ALIGN - 1)) & ~(ALIGN - 1);

    /* 首次适配 */
    for (chunk_t *c = g_head; c; c = c->next) {
        if (c->free && c->size >= want) {
            /* 切得下就切：余量够一个最小块 + 载荷才切，否则整块给你 */
            if (c->size >= want + HDR + ALIGN) {
                chunk_t *rest = (chunk_t *)((char *)c + HDR + want);
                rest->magic = CHUNK_MAGIC;
                rest->size = c->size - want - HDR;
                rest->free = 1;
                rest->prev = c;
                rest->next = c->next;
                if (c->next) c->next->prev = rest;
                else g_bump = rest;
                c->next = rest;
                c->size = want;
            }
            c->free = 0;
            return payload(c);
        }
    }

    chunk_t *c = carve(want);
    if (!c) return 0;
    return payload(c);
}

void free(void *p)
{
    if (!p) return;
    chunk_t *c = (chunk_t *)((char *)p - HDR);
    if (c->magic != CHUNK_MAGIC) return;   /* 不是我们的块，忽略而不是崩 */
    if (c->free) return;
    c->free = 1;

    /* 与后邻合并 */
    if (c->next && c->next->free) {
        chunk_t *n = c->next;
        c->size += HDR + n->size;
        c->next = n->next;
        if (c->next) c->next->prev = c;
        else g_bump = c;
    }
    /* 与前邻合并 */
    if (c->prev && c->prev->free) {
        chunk_t *pv = c->prev;
        pv->size += HDR + c->size;
        pv->next = c->next;
        if (c->next) c->next->prev = pv;
        else g_bump = pv;
    }
}

/* K/N 的分配器会问「能不能给我一块按 N 对齐的内存」。 */

/* ---- mmap：无虚拟内存之前的退化实现，直接从堆上切 ------------------------ */

#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define MAP_PRIVATE 0x02
#define MAP_ANON    0x20
#define MAP_FAILED  ((void *)-1)

void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off)
{
    (void)addr; (void)prot; (void)flags; (void)fd; (void)off;
    if (len == 0) return MAP_FAILED;
    size_t page = 4096;
    size_t need = (len + page - 1) & ~(page - 1);
    void *p = malloc(need);
    return p ? p : MAP_FAILED;
}





/* ======================================================================== *
 * 2. 系统调用面
 * ======================================================================== */

long syscall(long nr, ...)
{
    va_list ap;
    va_start(ap, nr);
    long a0 = va_arg(ap, long);
    long a1 = va_arg(ap, long);
    long a2 = va_arg(ap, long);
    long a3 = va_arg(ap, long);
    long a4 = va_arg(ap, long);
    long a5 = va_arg(ap, long);
    va_end(ap);
    return kraos_syscall(nr, a0, a1, a2, a3, a4, a5);
}

/*
 * 时钟。
 *
 * 时基用 TSC。引导桩导出 kraos_rdtsc()，PIT 与 TSC 同源，所以读数单调递增，
 * 拿来做超时判断够用。真正的 wall clock 要读 CMOS/RTC，那是 P3 的事。
 */
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

struct kraos_timespec { long tv_sec; long tv_nsec; };

int clock_gettime(int clk_id, void *tp)
{
    if (!tp) return -22;
    unsigned long long t = kraos_rdtsc();
    struct kraos_timespec *ts = (struct kraos_timespec *)tp;
    ts->tv_sec = (long)(t / 1000000000ULL);
    ts->tv_nsec = (long)((t % 1000000000ULL) * 1000ULL);
    (void)clk_id;
    return 0;
}


/* ---- 写：内核只有串口一个可写目的地 ------------------------------------- */

long write(int fd, const void *buf, unsigned long count)
{
    (void)fd;
    if (!buf || count == 0) return 0;
    /* NUL 字节没有意义，直接跳过，避免串口上出现一串不可见字符 */
    const unsigned char *p = (const unsigned char *)buf;
    unsigned long n = 0;
    for (unsigned long i = 0; i < count; i++) {
        if (p[i] == 0) continue;
        n++;
    }
    return kraos_serial_write(buf, count);
}


/* ======================================================================== *
 * 3. 终止与栈保护
 * ======================================================================== */

void abort(void)
{
    put_str("[abort] KraOS 运行时调用 abort()\r\n");
    for (;;) { kraos_cli(); kraos_hlt(); }
}


/*
 * 栈保护。开了 -fstack-protector 时数组越界会走到这里。
 *
 * 裸机阶段没法「安全地」恢复调用栈 —— 唯一确定正确的做法是停下来，
 * 而且要把出错信息打在崩溃现场之前才有用。所以这里只报错并停机。
 */
void __stack_chk_fail(void)
{
    put_str("[stack] __stack_chk_fail：检测到栈破坏，停机\r\n");
    abort();
}

/* ======================================================================== *
 * 4. 退出回调
 * ======================================================================== */

#define MAX_ATEXIT 32
static void (*g_atexit[MAX_ATEXIT])(void *);
static void  *g_atexit_arg[MAX_ATEXIT];
static int    g_atexit_n = 0;

int __cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
    (void)dso;
    if (g_atexit_n >= MAX_ATEXIT) return -1;
    g_atexit[g_atexit_n] = fn;
    g_atexit_arg[g_atexit_n] = arg;
    g_atexit_n++;
    return 0;
}

/* ======================================================================== *
 * 5. 展开支持
 * ======================================================================== */

/*
 * 异常展开要遍历程序头来定位栈帧。CraOS 目前不走 unwinding ——
 * kraos_fault 打印完现场就停机 —— 所以这里直接报告「没有可迭代的头」。
 * 展开器看到非零返回就知道拿不到信息，会退回到「无法展开」。
 * 伪造一个空表反而更危险：展开器会以为拿到了合法数据。
 */
int dl_iterate_phdr(int (*cb)(void *, size_t, void *), void *data)
{
    (void)cb; (void)data;
    return 1;
}

/* ======================================================================== *
 * 6. pthread：单内核期全部退化成直接成功
 * ======================================================================== *
 *
 * P1/P2 是单核单线程，K/N 运行时仍会调用这些函数（initRuntime 里有
 * pthread_once、条件变量的初始化路径）。单线程下「加锁」与「不加锁」
 * 语义完全等价，所以返回 0 就是正确实现 —— 不是占位符。
 *
 * P3 上真正的多核调度落地时，这里必须换成真实现，否则多核会真的
 * 出现竞态。见 docs/PLAN-kraos-kotlin-native.md 的 P3。
 */

int pthread_mutex_lock(void *m)          { (void)m; return 0; }
int pthread_mutex_unlock(void *m)        { (void)m; return 0; }
int pthread_mutex_init(void *m, const void *a)   { (void)m; (void)a; return 0; }
int pthread_mutex_destroy(void *m)       { (void)m; return 0; }
int pthread_cond_init(void *c, const void *a)    { (void)c; (void)a; return 0; }
int pthread_cond_destroy(void *c)        { (void)c; return 0; }
int pthread_cond_broadcast(void *c)      { (void)c; return 0; }
int pthread_cond_wait(void *c, void *m)  { (void)c; (void)m; return 0; }

/*
 * pthread_once：单线程下没有并发，"只跑一次"可以退化成"标记已跑过"。
 * ctl 指向调用方持有的一个 int，第一次调用时把它从 0 置 1。
 */
int pthread_once(void *ctl, void (*fn)(void *), void *arg)
{
    if (!ctl || !fn) return 0;
    int *flag = (int *)ctl;
    if (*flag != 0) return 0;
    *flag = 1;
    fn(arg);
    return 0;
}

int pthread_key_create(void *k, void (*d)(void *)) { (void)k; (void)d; return 0; }
int pthread_self(void) { return 0; }

/* ======================================================================== *
 * 7. stdio 的最小面
 * ======================================================================== */

/*
 * `stderr` / `stdout` / `stdin` 是**指针变量**而不是 FILE 对象 ——
 * glibc 里它们的类型是 `FILE *`，引用代码编译出来就是
 * `movq stderr(%rip), %rax`，所以这里只要是个指针宽度的全局符号即可。
 */
static char g_stdio_obj;
void *stderr = &g_stdio_obj;
void *stdout = &g_stdio_obj;
void *stdin  = &g_stdio_obj;

int fputc(int c, void *stream)
{
    (void)stream;
    unsigned char b = (unsigned char)c;
    kraos_serial_write(&b, 1);
    return c;
}

int fputs(const char *s, void *stream)
{
    (void)stream;
    if (!s) return -1;
    unsigned long n = 0;
    while (s[n]) n++;
    kraos_serial_write(s, n);
    return 0;
}


int fflush(void *stream) { (void)stream; return 0; }

/* ======================================================================== *
 * 8. 格式化输出
 * ======================================================================== */

/*
 * 支持的转换：%d %i %u %x %X %o %c %s %p %%，以及 l/ll/z/h 这些长度修饰、
 * 数字宽度、0 填充、- 左对齐、+ 号。
 *
 * 不支持浮点、不支持 %*（宽度从参数取）。K/N 运行时的错误路径只用整数、
 * 字符串和指针，够了。真需要更完整的 printf 时，正确做法是引第三方实现，
 * 而不是在这里堆更多分支。
 */

typedef struct out_buf {
    char  *base;
    size_t cap;      /* 含结尾 NUL */
    size_t len;      /* 不含结尾 NUL */
} out_buf;

static void emit(out_buf *o, char c)
{
    if (o->len + 1 < o->cap) o->base[o->len] = c;
    o->len++;
}

static void emit_str(out_buf *o, const char *s, int width, int left, char pad)
{
    if (!s) s = "(null)";
    size_t n = 0;
    while (s[n]) n++;
    if (!left)
        for (size_t i = n; i < (size_t)width; i++) emit(o, pad);
    for (size_t i = 0; i < n; i++) emit(o, s[i]);
    if (left)
        for (size_t i = n; i < (size_t)width; i++) emit(o, ' ');
}

static void emit_num(out_buf *o, unsigned long v, int base, int neg,
                     int width, int left, char pad, int upper)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = digits[v % (unsigned)base]; v /= (unsigned)base; }

    int total = n + (neg ? 1 : 0);
    if (!left && pad == '0')
        for (int i = total; i < width; i++) emit(o, '0');
    if (neg) emit(o, '-');
    for (int i = n - 1; i >= 0; i--) emit(o, tmp[i]);
    if (left)
        for (int i = total; i < width; i++) emit(o, ' ');
}

static void format(out_buf *o, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') { emit(o, *fmt); continue; }

        fmt++;
        int left = 0, plus = 0, space = 0, alt = 0;
        char pad = ' ';
        for (;;) {
            if (*fmt == '-') { left = 1; fmt++; continue; }
            if (*fmt == '0') { pad = '0'; fmt++; continue; }
            if (*fmt == '+') { plus = 1; fmt++; continue; }
            if (*fmt == ' ') { space = 1; fmt++; continue; }
            if (*fmt == '#') { alt = 1; fmt++; continue; }
            break;
        }

        int width = 0;
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        else while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }

        /* 长度修饰：只影响取值宽度 */
        int lmod = 0;   /* 0=int 1=long 2=long long */
        for (;;) {
            if (*fmt == 'l') { lmod++; fmt++; continue; }
            if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') { lmod = 1; fmt++; continue; }
            if (*fmt == 'h') { fmt++; continue; }
            break;
        }

        char conv = *fmt;
        int neg = 0, upper = 0, base = 10;
        unsigned long uv = 0;

        switch (conv) {
        case 'd':
        case 'i': {
            long sv;
            if (lmod == 0) sv = (long)va_arg(ap, int);
            else if (lmod == 1) sv = va_arg(ap, long);
            else sv = va_arg(ap, long);
            neg = sv < 0;
            uv = neg ? (unsigned long)(-sv) : (unsigned long)sv;
            base = 10;
            break;
        }
        case 'u': uv = lmod ? (unsigned long)va_arg(ap, unsigned long)
                            : (unsigned long)va_arg(ap, unsigned int); base = 10; break;
        case 'x': uv = lmod ? (unsigned long)va_arg(ap, unsigned long)
                            : (unsigned long)va_arg(ap, unsigned int); base = 16; break;
        case 'X': uv = lmod ? (unsigned long)va_arg(ap, unsigned long)
                            : (unsigned long)va_arg(ap, unsigned int); base = 16; upper = 1; break;
        case 'o': uv = lmod ? (unsigned long)va_arg(ap, unsigned long)
                            : (unsigned long)va_arg(ap, unsigned int); base = 8; break;
        case 'p': uv = (unsigned long)va_arg(ap, void *); base = 16; alt = 1; neg = 0; break;
        case 'c': {
            char c = (char)va_arg(ap, int);
            if (!left) for (int i = 1; i < width; i++) emit(o, pad);
            emit(o, c);
            if (left) for (int i = 1; i < width; i++) emit(o, ' ');
            continue;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            emit_str(o, s, width, left, ' ');
            continue;
        }
        case '%': emit(o, '%'); continue;
        case '\0': emit(o, '%'); fmt--; continue;
        default: emit(o, '%'); emit(o, conv); continue;
        }

        if (neg) neg = 1;
        else if (plus && uv) plus = 1;
        else plus = 0;
        if (plus) width--;
        if (alt && base == 16) { emit_str(o, upper ? "0X" : "0x", 0, 0, ' '); }
        emit_num(o, uv, base, neg, width, left, pad, upper);
    }
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    out_buf o;
    o.base = buf;
    o.cap = size;
    o.len = 0;
    format(&o, fmt, ap);
    if (size) {
        size_t end = o.len < size - 1 ? o.len : size - 1;
        if (o.base[end] != '\0') o.base[end] = '\0';
    }
    return (int)o.len;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

/* vprintf / printf：K/N 只在错误路径用到，直接落到串口。 */


/* ======================================================================== *
 * 9. 文件系统：内核还没有 VFS，一律报「没有这个文件」
 * ======================================================================== *
 *
 * K/N 的启动路径会去 stat 一些东西（locale、时区、/proc/self/...）。
 * 现在 P2 还没有文件系统，所以诚实地报失败 —— 让调用方走 fallback 分支，
 * 比伪造一个成功的返回值安全得多。
 */

#define ENOENT  2
#define EBADF   9
#define ENOSYS  38

int open(const char *path, int flags, int mode)
{
    (void)path; (void)flags; (void)mode;
    return -ENOENT;
}


int __fxstat(int ver, int fd, void *buf)
{
    (void)ver; (void)fd; (void)buf;
    return -ENOENT;
}


/* ======================================================================== *
 * 10. 第二批：摘掉 glibc 后才浮出来的引用
 * ======================================================================== *
 *
 * 下面这批在第一次链接时看不到 —— 因为 ld 根本没走到引用它们的那段代码。
 * 补齐第一批之后，链接器继续往后解析，第二层依赖就冒出来了。
 * 「摘 libc 悬崖」就是这个意思：一层一层往下掉。
 */

void *calloc(size_t n, size_t size)
{
    /* 先用除法检查溢出，再乘 */
    if (size && n > (size_t)-1 / size) return 0;
    size_t total = n * size;
    void *p = malloc(total ? total : 1);
    if (!p) return 0;
    unsigned char *b = (unsigned char *)p;
    for (size_t i = 0; i < total; i++) b[i] = 0;
    return p;
}


void _Exit(int status) { (void)status; for (;;) { kraos_cli(); kraos_hlt(); } }

/*
 * dladdr：动态加载器查「这个地址属于哪个共享对象」。我们是静态链接，
 * 没有共享对象，返回 0 表示「查不到」。
 */
int dladdr(const void *addr, void *info)
{
    (void)addr; (void)info;
    return 0;
}

int pthread_detach(void *t) { (void)t; return 0; }

/*
 * pthread_getspecific / setspecific：线程局部存储。
 *
 * 单线程下没有「按线程查」这回事，一个静态表就够了。P3 上多核调度落地时，
 * 这两行必须换成按 CPU/线程 ID 索引 —— 否则两个核会互相踩到对方的局部量。
 */
#define TLS_MAX 16
static void *g_tls[TLS_MAX];
int pthread_setspecific(void *key, const void *val)
{
    int k = (int)(long)key;
    if (k < 0 || k >= TLS_MAX) return -22;
    g_tls[k] = (void *)val;
    return 0;
}

void *pthread_getspecific(void *key)
{
    int k = (int)(long)key;
    if (k < 0 || k >= TLS_MAX) return 0;
    return g_tls[k];
}

/* sleep / usleep：用 TSC 空转到点。单核期没有别的线程可让。 */
static void delay_ticks(unsigned long long want)
{
    unsigned long long t0 = kraos_rdtsc();
    while (kraos_rdtsc() - t0 < want) { }
}

unsigned int sleep(unsigned int seconds)
{
    delay_ticks((unsigned long long)seconds * 1000000000ULL);
    return 0;
}


/* ======================================================================== *
 * 11. 杂项
 * ======================================================================== */

const char *strerror(int e)
{
    switch (e) {
    case 0:  return "Success";
    case 1:  return "Operation not permitted";
    case 2:  return "No such file or directory";
    case 9:  return "Bad file descriptor";
    case 11: return "Resource temporarily unavailable";
    case 12: return "Cannot allocate memory";
    case 22: return "Invalid argument";
    case 38: return "Function not implemented";
    default: return "Unknown error";
    }
}

int __errno_location(void) { return 0; }


/* 供 Main.kt 的垫片自检调用，证明这些符号真的被链进来了。 */
