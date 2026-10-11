@file:OptIn(kotlinx.cinterop.ExperimentalForeignApi::class, kotlin.experimental.ExperimentalNativeApi::class)

package kraos.console

import kraos.platform.bytePtr
import kotlinx.cinterop.get
import kotlin.experimental.ExperimentalNativeApi

/*
 * 串口控制台。
 *
 * 0x3F8 是 PC 标准 COM1。entry.S 的 32 位启动桩已经把它配成 8N1，
 * 这里只负责发。LSR(0x3FD) 的 bit5 = THR empty，置位表示可以写下一个字节。
 */

private const val COM1: Int = 0x3F8
private const val LSR: Int = COM1 + 5
private const val THR: Int = COM1

/** 等待发送保持寄存器空。给一个有界上界，避免波特率异常时把内核挂死。 */
private fun waitTxEmpty() {
    var spins = 0
    while (readBytePort(LSR) and 0x20 == 0) {
        spins++
        if (spins > 1_000_000) return
    }
}

private fun putByte(b: Int) {
    waitTxEmpty()
    writeBytePort(THR, b and 0xFF)
}

/**
 * 输出一个字符，按 UTF-8 编码。
 *
 * 串口是字节流，不是字符流。Kotlin 的 Char 是 UTF-16 码元，直接发
 * `c.code and 0xFF` 会把中文砍成低字节 —— 「内核启动」(U+5185 U+6838
 * U+542F U+52A8) 只会变成 85 38 2f a8，四个字全毁。非 ASCII 字符必须
 * 编成 1~3 个字节再发。
 */
fun putChar(c: Char) {
    val code = c.code
    when {
        code < 0x80 -> putByte(code)
        code < 0x800 -> {
            putByte(0xC0 or (code shr 6))
            putByte(0x80 or (code and 0x3F))
        }
        else -> {
            putByte(0xE0 or (code shr 12))
            putByte(0x80 or ((code shr 6) and 0x3F))
            putByte(0x80 or (code and 0x3F))
        }
    }
}

fun write(text: String) {
    for (c in text) putChar(c)
}

/** 写十进制数。单独起名而不重载 write，避免与 String 版本产生歧义。 */
fun writeNumber(value: Long) = write(value.toString())

fun writeNumber(value: Int) = write(value.toString())

fun writeLine(text: String) {
    write(text)
    putChar('\n')
    putChar('\r')
}

fun writeHex(value: Long, digits: Int = 16) {
    val hex = "0123456789abcdef"
    write("0x")
    for (i in digits - 1 downTo 0) {
        putChar(hex[((value shr (i * 4)) and 0xF).toInt()])
    }
}
