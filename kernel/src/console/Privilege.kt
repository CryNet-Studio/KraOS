@file:OptIn(
    kotlinx.cinterop.ExperimentalForeignApi::class,
    kotlin.experimental.ExperimentalNativeApi::class
)

package kraos.console

import kraos.boot.kraos_cli
import kraos.boot.kraos_hlt
import kraos.boot.kraos_inb
import kraos.boot.kraos_outb
import kraos.boot.kraos_sti

/*
 * 特权操作。
 *
 * IO 端口读写和 cli/hlt/sti 在指令集层面没有对应的 Kotlin 表达，而这一版
 * Kotlin/Native（2.1.255）的内联汇编不可用（asm / asParam 均无法解析），
 * 所以它们以 C ABI 留在启动桩里，这里只是把它包成有语义的名字。
 *
 * 这是整个内核唯一不是纯 Kotlin 的部分：一个约 300 行的 .bss 引导桩。
 */

/** 往 IO 端口写一个字节。 */
fun writeBytePort(port: Int, value: Int) = kraos_outb(port, value)

/** 从 IO 端口读一个字节（0..255）。 */
fun readBytePort(port: Int): Int = kraos_inb(port)

/** 关中断。进入临界区时用。 */
fun disableInterrupts() = kraos_cli()

/** 开中断。调度器初始化完成后会用到。 */
fun enableInterrupts() = kraos_sti()

/** 停机等待中断。P3 调度器落地前，这是内核终止自己的方式。 */
fun haltForever() {
    while (true) {
        kraos_cli()
        kraos_hlt()
    }
}
