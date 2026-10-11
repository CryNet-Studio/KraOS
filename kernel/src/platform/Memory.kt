package kraos.platform

import kotlinx.cinterop.ByteVar
import kotlinx.cinterop.COpaquePointer
import kotlinx.cinterop.CPointer
import kotlinx.cinterop.ExperimentalForeignApi
import kotlinx.cinterop.LongVar
import kotlinx.cinterop.IntVar
import kotlinx.cinterop.get
import kotlinx.cinterop.plus
import kotlinx.cinterop.interpretCPointer
import kotlinx.cinterop.set
import kotlin.experimental.ExperimentalNativeApi
import kotlin.native.internal.NativePtr

/*
 * 裸地址访问。
 *
 * Kotlin/Native 里 `Long -> CPointer` 没有直接的 reinterpret 重载；
 * `interpretCPointer` 收的是 NativePtr，而 NativePtr 的构造函数是 internal，
 * 但 `NativePtr.NULL + offset` 是公开的、带 TypedIntrinsic 的运算，正好用来
 * 造出任意地址的裸指针。
 *
 * 这层封装是全项目唯一碰 cinterop 的地方，其余代码一律用 Long 地址 + 本文件
 * 的读写函数，避免每个文件都重复那串 OptIn 与 import。
 */

/** 把线性地址变成字节指针。 */
@OptIn(ExperimentalForeignApi::class, ExperimentalNativeApi::class)
fun bytePtr(addr: Long): CPointer<ByteVar> =
    interpretCPointer(NativePtr.NULL + addr)!!

/** 把线性地址变成 8 字节对齐的字指针。 */
@OptIn(ExperimentalForeignApi::class, ExperimentalNativeApi::class)
fun longPtr(addr: Long): CPointer<LongVar> =
    interpretCPointer(NativePtr.NULL + addr)!!

/** 把线性地址变成 32 位像素指针。 */
@OptIn(ExperimentalForeignApi::class, ExperimentalNativeApi::class)
fun intPtr(addr: Long): CPointer<IntVar> =
    interpretCPointer(NativePtr.NULL + addr)!!

/** 读一个字节（0..255）。 */
@OptIn(ExperimentalForeignApi::class, ExperimentalNativeApi::class)
fun readByte(addr: Long): Int = bytePtr(addr)[0].toInt() and 0xFF

/** 写一个字节。 */
@OptIn(ExperimentalForeignApi::class, ExperimentalNativeApi::class)
fun writeByte(addr: Long, value: Int) {
    bytePtr(addr)[0] = value.toByte()
}

/** 小端 32 位读。 */
@OptIn(ExperimentalForeignApi::class)
fun readU32(addr: Long): Long {
    val p = bytePtr(addr)
    return (p[0].toLong() and 0xFFL) or
        ((p[1].toLong() and 0xFFL) shl 8) or
        ((p[2].toLong() and 0xFFL) shl 16) or
        ((p[3].toLong() and 0xFFL) shl 24)
}

/** 小端 64 位读。 */
@OptIn(ExperimentalForeignApi::class)
fun readU64(addr: Long): Long = readU32(addr) or (readU32(addr + 4) shl 32)

/** 小端 32 位写。 */
@OptIn(ExperimentalForeignApi::class)
fun writeU32(addr: Long, value: Long) {
    val p = bytePtr(addr)
    p[0] = (value and 0xFF).toByte()
    p[1] = ((value shr 8) and 0xFF).toByte()
    p[2] = ((value shr 16) and 0xFF).toByte()
    p[3] = ((value shr 24) and 0xFF).toByte()
}

/** 小端 64 位写。 */
@OptIn(ExperimentalForeignApi::class)
fun writeU64(addr: Long, value: Long) {
    writeU32(addr, value)
    writeU32(addr + 4, value shr 32)
}

/** 把 [length] 字节内存清零，按 8 字节批量写。 */
@OptIn(ExperimentalForeignApi::class)
fun zeroMemory(start: Long, length: Long) {
    val words = longPtr(start)
    val n = (length / 8).toInt()
    for (i in 0 until n) words[i] = 0L
    var tail = start + n * 8L
    val end = start + length
    if (tail < end) {
        val p = bytePtr(tail)
        var i = 0
        while (tail < end) {
            p[i] = 0
            tail++
            i++
        }
    }
}
/** 往 COpaquePointer 指向的地址写 8 字节。运行时垫片按字节拼结构体时用。 */
@OptIn(ExperimentalForeignApi::class)
fun writeU64ToPtr(ptr: COpaquePointer, offset: Long, value: Long) {
    writeU64(ptr.rawValue.toLong() + offset, value)
}
