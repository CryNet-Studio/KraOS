@file:OptIn(kotlinx.cinterop.ExperimentalForeignApi::class, kotlin.experimental.ExperimentalNativeApi::class)

package kraos.boot

/*
 * 引导期内存。
 *
 * 真正的分配器在 boot/libc_shim.c 里 —— 必须是 C，理由见那个文件的注释：
 * Kotlin 的 @CName 导出只有在 K/N 运行时位码引用了该符号时才会生成对外可见的
 * 全局符号，glibc 静态库内部对 __brk/__mmap 的调用不在 K/N 的视野里，Kotlin
 * 侧同名实现要么不生成、要么被 -opt 内联掉。
 *
 * 这个 object 只是 Kotlin 侧的薄门面：所有状态与策略都在 C 那边，这里通过
 * cinterop 绑定（boot.def）调用，因此 Kotlin 与 C 看到的是同一块内存，
 * 不存在两套游标互相踩踏的问题。
 *
 * P2 会把 C 侧的 bump 换成解析 Multiboot2 内存图得到的物理页管理器；
 * Kotlin 这一层因为只是转发，不需要跟着改。
 */

class OutOfMemoryError(message: String) : RuntimeException(message)

object BootHeap {
    /** 引导堆下界（glibc malloc 那半的起点）。 */
    val heapStart: Long get() = kraos_heap_start()

    /** 引导堆上界（Kotlin/K/N mmap 那半的终点）。 */
    val heapEnd: Long get() = kraos_heap_end()

    /** glibc malloc 的当前断点。 */
    val breakValue: Long get() = kraos_break_value()

    val bytesAllocated: Long get() = kraos_alloc_bytes()
    val allocations: Long get() = kraos_alloc_count()

    /** 分配 [size] 字节（内部按 16 字节对齐并清零），失败返回 0。 */
    fun allocate(size: Long): Long {
        if (size <= 0) throw OutOfMemoryError("非法分配大小: $size")
        val p = kraos_alloc(size)
        if (p == 0L) {
            throw OutOfMemoryError(
                "引导堆耗尽: 需要 $size 字节, 已分配 ${kraos_alloc_bytes()} 字节" +
                    "（${kraos_alloc_count()} 次）, 上限 ${kraos_heap_end()}"
            )
        }
        return p
    }
}
