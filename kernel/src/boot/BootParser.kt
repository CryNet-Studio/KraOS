@file:OptIn(kotlinx.cinterop.ExperimentalForeignApi::class, kotlin.experimental.ExperimentalNativeApi::class)

package kraos.boot

import kraos.platform.bytePtr
import kotlinx.cinterop.get
import kotlinx.cinterop.set
import kotlin.experimental.ExperimentalNativeApi

/*
 * 引导信息解析。
 *
 * Multiboot2 把一个「信息结构体」交给内核，开头是 total_size / reserved，
 * 后面跟一串 (tag, size) 定长记录。TAG_MMAP 那条里又是一串内存区描述。
 *
 * 全部按裸地址 + 小端字节拼读，不让 cinterop 去映射结构体：引导信息里的字段
 * 本来就是按字节对齐的定长布局，手工偏移比让绑定生成器猜对齐更可控，也避免
 * 给 freestanding 目标引入额外的结构体 ABI 假设。
 */

/** 解析结果：内存区列表，以及可选的 framebuffer。 */
class BootInfo(
    val memoryRegions: List<MemoryRegion>,
    val framebuffer: Framebuffer?,
)

class MemoryRegion(val base: Long, val length: Long, val type: Int)

class Framebuffer(
    val base: Long,
    val pitch: Int,
    val width: Int,
    val height: Int,
    val bpp: Int,
    val type: Int,
    val redPosition: Int,
    val redSize: Int,
    val greenPosition: Int,
    val greenSize: Int,
    val bluePosition: Int,
    val blueSize: Int,
)

/**
 * 解析 Multiboot2 信息结构体。
 *
 * @param info 线性地址，由 entry.S 里的 kraos_mb_info() 返回
 * @return 解析结果；地址为空或长度不合理时返回 null
 */
@OptIn(ExperimentalNativeApi::class)
fun parseBootInfo(info: Long): BootInfo? {
    if (info == 0L) return null

    val totalSize = kraos.platform.readU32(info)
    if (totalSize < 8) return null

    val regions = ArrayList<MemoryRegion>()
    var fb: Framebuffer? = null

    // 逐条走定长记录。每条至少 8 字节头（tag:4 + size:4），记录按 8 字节对齐。
    var offset = 8L
    while (offset + 8 <= totalSize) {
        val tag = kraos.platform.readU32(info + offset).toInt()
        val size = kraos.platform.readU32(info + offset + 4)
        if (size < 8 || offset + size > totalSize) break

        when (tag) {
            TAG_MMAP -> parseMmap(info + offset + 8, size - 8, regions)
            TAG_FRAMEBUFFER -> fb = parseFramebuffer(info + offset + 8, size - 8)
        }
        offset += (size + 7) and 7L.inv().toLong()
    }

    return BootInfo(regions, fb)
}

@OptIn(kotlinx.cinterop.ExperimentalForeignApi::class, kotlin.experimental.ExperimentalNativeApi::class)
private fun parseMmap(base: Long, size: Long, out: MutableList<MemoryRegion>) {
    /*
     * Multiboot2 的 mmap 标签载荷开头还有 8 字节的头：
     *   u32 entry_size    每条描述符的字节数（规范规定是 24）
     *   u32 entry_version 当前是 0
     * 之后才是连续的 (base:u64, length:u64, type:u32, reserved:u32)。
     *
     * 之前从偏移 0 就开始当描述符读，等于把 8 字节头当成第一条的 base/len，
     * 于是每个字段都错位，读出来的「type」全是地址碎片，可用内存也算成 0。
     * 步长也用标签自报的 entry_size，而不是写死 24。
     */
    if (size < 8) return
    val entrySize = kraos.platform.readU32(base).toInt()
    if (entrySize < 24) return

    var offset = 8L
    while (offset + entrySize <= size) {
        val entryBase = kraos.platform.readU64(base + offset)
        val length = kraos.platform.readU64(base + offset + 8)
        val type = kraos.platform.readU32(base + offset + 16).toInt()
        if (length > 0) out.add(MemoryRegion(entryBase, length, type))
        offset += entrySize
    }
}

@OptIn(ExperimentalNativeApi::class)
private fun parseFramebuffer(base: Long, size: Long): Framebuffer? {
    // Multiboot2 framebuffer tag: 30-byte fixed payload followed by optional palette.
    // RGB channel positions/sizes occupy payload offsets 24..29 (single bytes).
    if (size < 30) return null
    val fb = Framebuffer(
        base = kraos.platform.readU64(base),
        pitch = kraos.platform.readU32(base + 8).toInt(),
        width = kraos.platform.readU32(base + 12).toInt(),
        height = kraos.platform.readU32(base + 16).toInt(),
        bpp = kraos.platform.readByte(base + 20),
        type = kraos.platform.readByte(base + 21),
        redPosition = kraos.platform.readByte(base + 24),
        redSize = kraos.platform.readByte(base + 25),
        greenPosition = kraos.platform.readByte(base + 26),
        greenSize = kraos.platform.readByte(base + 27),
        bluePosition = kraos.platform.readByte(base + 28),
        blueSize = kraos.platform.readByte(base + 29),
    )
    if (fb.base == 0L || fb.width !in 640..4096 || fb.height !in 480..2160 ||
        fb.bpp != 32 || fb.type != 1 || fb.pitch < fb.width * 4 || (fb.pitch and 3) != 0 ||
        fb.redSize != 8 || fb.greenSize != 8 || fb.blueSize != 8 ||
        fb.redPosition > 24 || fb.greenPosition > 24 || fb.bluePosition > 24) return null
    val r = 255L shl fb.redPosition
    val g = 255L shl fb.greenPosition
    val b = 255L shl fb.bluePosition
    if ((r and g) != 0L || (r and b) != 0L || (g and b) != 0L) return null
    return fb
}

/** 往线性地址写一段字节，供 framebuffer 与共享内存使用。 */
@OptIn(ExperimentalNativeApi::class)
fun writeBytes(target: Long, bytes: ByteArray, count: Int = bytes.size) {
    val p = bytePtr(target)
    for (i in 0 until count) p[i] = bytes[i]
}