package kraos.desktop

/** Decode the BIOS-translated set-1 keyboard stream, multiplexed by Input.poll(). */
internal object Keyboard {
    private var pendingMake = -1
    private var extended = false
    private var leftShift = false
    private var rightShift = false
    private var capsLock = false
    private var capsDown = false

    val hasPending: Boolean get() = pendingMake >= 0

    fun acceptByte(code: Int) {
        if (code == 0xe0 || code == 0xe1) {
            extended = true
            return
        }
        val released = (code and 0x80) != 0
        val scan = code and 0x7f
        if (!extended) {
            when (scan) {
                0x2a -> { leftShift = !released; return }
                0x36 -> { rightShift = !released; return }
                0x3a -> {
                    if (!released && !capsDown) capsLock = !capsLock
                    capsDown = !released
                    return
                }
            }
        }
        if (!released && !extended && pendingMake < 0) pendingMake = scan
        extended = false
    }

    /** Returns ASCII/control keys, or -1 when no key is pending. */
    fun pollKey(): Int {
        val scan = pendingMake
        if (scan < 0) return -1
        pendingMake = -1
        return translate(scan)
    }

    private fun translate(scan: Int): Int {
        val shifted = leftShift || rightShift
        val letter = when (scan) {
            0x1e -> 'a'; 0x30 -> 'b'; 0x2e -> 'c'; 0x20 -> 'd'; 0x12 -> 'e'
            0x21 -> 'f'; 0x22 -> 'g'; 0x23 -> 'h'; 0x17 -> 'i'; 0x24 -> 'j'
            0x25 -> 'k'; 0x26 -> 'l'; 0x32 -> 'm'; 0x31 -> 'n'; 0x18 -> 'o'
            0x19 -> 'p'; 0x10 -> 'q'; 0x13 -> 'r'; 0x1f -> 's'; 0x14 -> 't'
            0x16 -> 'u'; 0x2f -> 'v'; 0x11 -> 'w'; 0x2d -> 'x'; 0x15 -> 'y'; 0x2c -> 'z'
            else -> null
        }
        if (letter != null) return if (shifted xor capsLock) letter.uppercaseChar().code else letter.code
        val normal = when (scan) {
            0x01 -> 27; 0x0e -> 8; 0x0f -> 9; 0x1c -> 10; 0x39 -> 32
            0x02 -> '1'.code; 0x03 -> '2'.code; 0x04 -> '3'.code; 0x05 -> '4'.code
            0x06 -> '5'.code; 0x07 -> '6'.code; 0x08 -> '7'.code; 0x09 -> '8'.code
            0x0a -> '9'.code; 0x0b -> '0'.code; 0x0c -> '-'.code; 0x0d -> '='.code
            0x1a -> '['.code; 0x1b -> ']'.code; 0x2b -> '\\'.code
            0x27 -> ';'.code; 0x28 -> '\''.code; 0x29 -> '`'.code
            0x33 -> ','.code; 0x34 -> '.'.code; 0x35 -> '/'.code
            else -> return -1
        }
        if (!shifted) return normal
        return when (normal) {
            '1'.code -> '!'.code; '2'.code -> '@'.code; '3'.code -> '#'.code
            '4'.code -> '$'.code; '5'.code -> '%'.code; '6'.code -> '^'.code
            '7'.code -> '&'.code; '8'.code -> '*'.code; '9'.code -> '('.code
            '0'.code -> ')'.code; '-'.code -> '_'.code; '='.code -> '+'.code
            '['.code -> '{'.code; ']'.code -> '}'.code; '\\'.code -> '|'.code
            ';'.code -> ':'.code; '\''.code -> '"'.code; '`'.code -> '~'.code
            ','.code -> '<'.code; '.'.code -> '>'.code; '/'.code -> '?'.code
            else -> normal
        }
    }

    /** Number-row desktop shortcuts: 1..5 open apps, Escape returns to Overview. */
    fun desktopApp(key: Int): Int? = when (key) {
        27 -> 0
        '1'.code -> 1
        '2'.code -> 2
        '3'.code -> 3
        '4'.code -> 4
        '5'.code -> 5
        else -> null
    }
}
