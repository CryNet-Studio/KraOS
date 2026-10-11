package kraos.desktop

/** Decode the BIOS-translated set-1 keyboard stream, multiplexed by Input.poll(). */
internal object Keyboard {
    private var pendingMake = -1
    private var extended = false

    val hasPending: Boolean get() = pendingMake >= 0

    fun acceptByte(code: Int) {
        if (code == 0xe0 || code == 0xe1) {
            extended = true
            return
        }
        if ((code and 0x80) != 0) {
            extended = false
            return
        }
        if (!extended && pendingMake < 0) pendingMake = code
        extended = false
    }

    fun pollMakeCode(): Int {
        val result = pendingMake
        pendingMake = -1
        return result
    }
    // Set-1 make codes: 1=Esc, 2..6=number-row keys 1..5.
    fun desktopApp(code: Int): Int? = when (code) {
        1 -> 0
        2 -> 1 // Terminal
        3 -> 2 // Files
        4 -> 3 // Notes
        5 -> 4 // Settings
        6 -> 5 // About
        else -> null
    }

    /** Returns -1 if no simple make code is waiting. */
    fun pollMakeCode(): Int {
        if ((readBytePort(0x64) and 1) == 0) return -1
        val code = readBytePort(0x60)
        // Ignore break codes and extended-key prefixes in this first shortcut-only slice.
        if ((code and 0x80) != 0 || code == 0xe0 || code == 0xe1) return -1
        return code
    }
}
