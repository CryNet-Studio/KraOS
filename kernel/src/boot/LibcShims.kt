@file:OptIn(kotlin.experimental.ExperimentalNativeApi::class)

package kraos.boot

/*
 * libc 垫片的存活锚点与启动自检。
 *
 * 这一层本身没有实现（实现全在 boot/libc_shim.c），存在的意义是让内核在启动
 * 时真跑一遍「Kotlin -> cinterop -> C 分配器」这条链路，确认 brk 与 mmap
 * 都接到了引导堆上，而不是接到一条会触发 #UD 的 syscall 指令上。
 */

object LibcShims {
    data class Report(
        val breakValue: Long,
        val mmapProbe: Long,
        val heapStart: Long,
        val heapEnd: Long,
    )

    fun selfCheck(): Report {
        // brk(0) 在 glibc 语义下就是「问当前断点」，不改动任何状态。
        val brk = kraos_brk_impl(0L)
        // 匿名映射一页，确认 mmap 改道确实接到了 bump 分配器。
        val probe = kraos_alloc(4096L)
        return Report(brk, probe, BootHeap.heapStart, BootHeap.heapEnd)
    }
}
