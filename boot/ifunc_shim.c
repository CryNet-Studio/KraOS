/*
 * kraos 自由字符串/内存实现 —— 顶掉 glibc 的 ifunc。
 *
 * 为什么需要这个文件：
 *
 *   K/N 的 linux_x64 运行时位码是按动态链接编的，里面对 memcpy、strlen、ceil
 *   这些符号的引用走 GOT。而本项目的 glibc 是带 ifunc 的版本：glibc 把每个函数
 *   定义成 STT_GNU_IFUNC，真正的实现在运行期由解析器（strcmp_ifunc 这类局部符号）
 *   按 CPU 特性挑一个填进 GOT。
 *
 *   链接静态内核时没有动态链接器去做这一步。ld.lld 会生成 .iplt 桩（桩体是
 *   「jmp *GOT 槽」），GOT 槽却留空 —— 它把 IRELATIVE 重定位消费掉了，既不留在
 *   输出里，静态可执行文件也没有别的地方会补。第一次走到 ifunc 调用（本项目里是
 *   libc++ 算 unordered_map 容量时的 ceil）就会跳进地址 0。
 *
 *   试过 --emit-relocs、显式保留 .rela.iplt、ld 侧的手写 IRELATIVE 循环：lld 对
 *   静态输出根本不再吐出这张表，__rela_iplt_start == __rela_iplt_end。
 *
 * 所以直接绕开：给这些符号提供自己的实现，用链接器的 --wrap 把所有对
 * <名字> 的引用换成 __wrap_<名字>。解析器于是根本不会被拉进链接，.iplt 自然为空。
 *
 * 取舍：牺牲一点性能（没有 SIMD 版本），换来内核不依赖任何 ifunc 解析路径。
 * 等 P2 真正把 glibc 摘掉之后，这里就是内核自带的 str/mem 实现，一直留着。
 *
 * 编译时用 -ffreestanding -fno-builtin，否则 gcc 会把下面这些循环重新变成对
 * memcpy/memset 的调用，自己调自己。
 */

typedef unsigned long kraos_size;
typedef unsigned long kraos_uintptr;

#define KRAOS_NULL ((void *)0)

/* ---- 内存块 ------------------------------------------------------------ */

void *__wrap_memcpy(void *dst, const void *src, kraos_size n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    while (n--) *d++ = *s++;
    return dst;
}

void *__wrap_memmove(void *dst, const void *src, kraos_size n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *__wrap_memset(void *dst, int c, kraos_size n)
{
    unsigned char *d = (unsigned char *)dst;
    unsigned char v = (unsigned char)c;

    while (n--) *d++ = v;
    return dst;
}



/* glibc 内部版本：命中即返回，不检查结尾处的 0。 */


/* 返回结尾 0 的下一地址，供 strcat 之类拼接用。 */

int __wrap_bcmp(const void *a, const void *b, kraos_size n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (n--) {
        if (*x++ != *y++) return 1;
    }
    return 0;
}

/* ---- 长度与拷贝 -------------------------------------------------------- */

kraos_size __wrap_strlen(const char *s)
{
    const char *p = s;

    while (*p) p++;
    return (kraos_size)(p - s);
}



char *__wrap_strncpy(char *dst, const char *src, kraos_size n)
{
    char *d = dst;

    while (n && *src) { *d++ = *src++; n--; }
    while (n--) *d++ = 0;
    return dst;
}





/* ---- 比较 -------------------------------------------------------------- */

int __wrap_strcmp(const char *a, const char *b)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;

    while (*x && *x == *y) { x++; y++; }
    return (int)*x - (int)*y;
}


static int lower_ascii(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}



/* ---- 查找 -------------------------------------------------------------- */


char *__wrap_strrchr(const char *s, int c)
{
    const char *last = KRAOS_NULL;
    char v = (char)c;

    for (;;) {
        if (*s == v) last = s;
        if (*s == 0) break;
        s++;
    }
    return (char *)last;
}

/* 永远不返回 NULL：找不到就返回结尾 0 的地址。 */







/* ---- 取整 --------------------------------------------------------------
 *
 * 只需要覆盖 |x| < 2^52 的区间，这个范围内 double 的整数部分能被 long long 精确
 * 表示。超出范围（含 NaN 与 ±inf）时 x 本身就是整数或非数，原样返回即可。
 */

double __wrap_ceil(double x)
{
    double t;

    if (!(x > -4503599627370496.0 && x < 4503599627370496.0)) return x;
    t = (double)(long long)x;
    if (t < x) t += 1.0;
    return t;
}

double __wrap_floor(double x)
{
    double t;

    if (!(x > -4503599627370496.0 && x < 4503599627370496.0)) return x;
    t = (double)(long long)x;
    if (t > x) t -= 1.0;
    return t;
}
/* ---- glibc 内部隐藏别名 ------------------------------------------------
 *
 * glibc 自己内部调用统一走 __ 前缀的隐藏别名（__memcpy、__rawmemchr …），
 * 这些别名指向同一批 ifunc。只 wrap 公开名不够：__strtod_l_internal 里那次
 * memcpy 调用走的就是 __memcpy，__gconv_open 同理。符号表上只多一层下划线，
 * 调用约定与公开名完全一致，多余参数（_l 变体的 locale）直接忽略。
 */







/* __memcmpeq 只判等，返回真/假，与 memcmp 的返回值语义不同。 */























/* _l 变体多一个 locale 参数，本内核不用，直接忽略。 */

