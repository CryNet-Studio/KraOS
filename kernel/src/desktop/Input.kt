package kraos.desktop

import kraos.console.readBytePort
import kraos.console.writeBytePort

/** Bounded polling of the already initialized i8042 controller. No IRQ path is required yet. */
internal object Input {
    private val packet = IntArray(3)
    private var packetIndex = 0
    var mouseX = 0
        private set
    var mouseY = 0
        private set
    var buttons = 0
        private set
    var mouseReady = false
        private set

    private fun waitWrite(): Boolean {
        repeat(10000) {
            if ((readBytePort(0x64) and 2) == 0) return true
        }
        return false
    }

    private fun writeController(command: Int): Boolean {
        if (!waitWrite()) return false
        writeBytePort(0x64, command)
        return true
    }

    private fun writeData(value: Int): Boolean {
        if (!waitWrite()) return false
        writeBytePort(0x60, value)
        return true
    }

    private fun readResponse(auxiliary: Boolean): Int {
        repeat(10000) {
            val status = readBytePort(0x64)
            if ((status and 1) == 0) return@repeat
            val value = readBytePort(0x60)
            if ((status and 0xc0) != 0) return@repeat
            if (((status and 0x20) != 0) == auxiliary) return value
        }
        return -1
    }

    private fun mouseCommand(command: Int): Boolean {
        repeat(3) {
            if (!writeController(0xd4) || !writeData(command)) return false
            val response = readResponse(true)
            if (response == 0xfa) return true
            if (response != 0xfe) return false
        }
        return false
    }

    fun initialize(width: Int, height: Int) {
        mouseX = width / 2
        mouseY = height / 2
        packetIndex = 0
        buttons = 0
        mouseReady = false

        // Enable the auxiliary port, enable its clock, and keep IRQ12 masked for polling.
        if (!writeController(0xa8) || !writeController(0x20)) return
        val config = readResponse(false)
        if (config < 0) return
        val pollingConfig = (config or 0x40) and 0xdd
        if (!writeController(0x60) || !writeData(pollingConfig)) return
        mouseReady = mouseCommand(0xf6) && mouseCommand(0xf4)
    }

    private fun acceptMouseByte(value: Int, width: Int, height: Int) {
        if (packetIndex == 0 && (value and 0x08) == 0) return
        packet[packetIndex++] = value
        if (packetIndex < 3) return
        packetIndex = 0
        val flags = packet[0]
        var dx = packet[1] - if ((flags and 0x10) != 0) 256 else 0
        var dy = packet[2] - if ((flags and 0x20) != 0) 256 else 0
        if ((flags and 0x40) != 0) dx = 0
        if ((flags and 0x80) != 0) dy = 0
        mouseX = (mouseX + dx).coerceIn(0, width - 1)
        mouseY = (mouseY - dy).coerceIn(0, height - 1)
        buttons = flags and 7
    }

    /** Consume at most 32 controller bytes, preserving queued keyboard makes for the caller. */
    fun poll(width: Int, height: Int) {
        repeat(32) {
            val status = readBytePort(0x64)
            if ((status and 1) == 0) return
            val auxiliary = (status and 0x20) != 0
            if (!auxiliary && Keyboard.hasPending) return
            val value = readBytePort(0x60)
            if ((status and 0xc0) != 0) return
            if (auxiliary) acceptMouseByte(value, width, height) else Keyboard.acceptByte(value)
        }
    }
}
