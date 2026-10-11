@file:OptIn(
    kotlinx.cinterop.ExperimentalForeignApi::class,
    kotlin.experimental.ExperimentalNativeApi::class
)

package kraos.boot

/*
 * Multiboot2 的常量，以及引导信息解析要用到的 tag。
 *
 * 引导桩导出的 kraos_* 函数不在这里声明：它们由 boot/boot.def 经 cinterop
 * 生成绑定（package = kraos.boot），本包内可直接调用。
 */

/** Multiboot2 引导器 magic，正常必须是这个值。 */
const val MULTIBOOT2_BOOTLOADER_MAGIC: Long = 0x36d76289L

/** 多头信息 tag：基础内存信息。 */
const val TAG_BASIC_MEMINFO: Int = 4

/** 多头信息 tag：引导加载器提供的内存图。 */
const val TAG_MMAP: Int = 6

/** 多头信息 tag：framebuffer。 */
const val TAG_FRAMEBUFFER: Int = 8

/** 内存图条目里「可用 RAM」的类型值。 */
const val MEMORY_AVAILABLE: Int = 1

/** 内存图条目长度：base_addr(8) + length(8) + type(4) + pad(4)。 */
const val MMAP_ENTRY_SIZE: Int = 24
