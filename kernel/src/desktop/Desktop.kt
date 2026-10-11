@file:OptIn(kotlinx.cinterop.ExperimentalForeignApi::class, kotlin.experimental.ExperimentalNativeApi::class)

package kraos.desktop

import kraos.boot.Framebuffer
import kraos.platform.intPtr
import kotlinx.cinterop.set

/** First framebuffer desktop surface: software-composited directly into the Multiboot2 RGB buffer. */
object Desktop {
    private lateinit var fb: Framebuffer
    private lateinit var pixels: kotlinx.cinterop.CPointer<kotlinx.cinterop.IntVar>
    private var stride = 0
    private var uiScale = 1
    private val logicalWidth get() = fb.width / uiScale
    private val logicalHeight get() = fb.height / uiScale
    private var activeApp = 0
    private var cursorX = 0
    private var cursorY = 0
    private var cursorDrawn = false
    private val cursorSaved = IntArray(16 * 20)

    private fun rgb(r: Int, g: Int, b: Int): Int =
        (r shl fb.redPosition) or (g shl fb.greenPosition) or (b shl fb.bluePosition)

    private fun physicalPixel(x: Int, y: Int, color: Int) {
        if (x in 0 until fb.width && y in 0 until fb.height) pixels[y * stride + x] = color
    }

    private fun cursorInside(x: Int, y: Int): Boolean =
        (y < 16 && x <= minOf(9, y / 2)) || (y in 11..18 && x in 4..8)

    private fun restoreCursor() {
        if (!cursorDrawn) return
        val left = cursorX * uiScale
        val top = cursorY * uiScale
        for (y in 0 until 20) for (x in 0 until 16) {
            physicalPixel(left + x, top + y, cursorSaved[y * 16 + x])
        }
        cursorDrawn = false
    }

    private fun drawCursor() {
        val left = (cursorX * uiScale).coerceIn(0, (fb.width - 16).coerceAtLeast(0))
        val top = (cursorY * uiScale).coerceIn(0, (fb.height - 20).coerceAtLeast(0))
        for (y in 0 until 20) for (x in 0 until 16) {
            cursorSaved[y * 16 + x] = if (left + x < fb.width && top + y < fb.height)
                pixels[(top + y) * stride + left + x] else 0
        }
        val edge = rgb(4, 10, 18)
        val fill = rgb(245, 249, 255)
        for (y in 0 until 20) for (x in 0 until 16) if (cursorInside(x, y)) {
            val border = !cursorInside(x - 1, y) || !cursorInside(x + 1, y) ||
                !cursorInside(x, y - 1) || !cursorInside(x, y + 1)
            physicalPixel(left + x, top + y, if (border) edge else fill)
        }
        cursorDrawn = true
    }

    private fun rect(x: Int, y: Int, w: Int, h: Int, color: Int) {
        val x0 = (x * uiScale).coerceAtLeast(0)
        val y0 = (y * uiScale).coerceAtLeast(0)
        val x1 = ((x + w) * uiScale).coerceAtMost(fb.width)
        val y1 = ((y + h) * uiScale).coerceAtMost(fb.height)
        if (x0 >= x1 || y0 >= y1) return
        for (row in y0 until y1) {
            val start = row * stride + x0
            for (col in x0 until x1) pixels[row * stride + col] = color
        }
    }

    private fun text(x: Int, y: Int, value: String, color: Int, scale: Int = 1, maxChars: Int = 128) {
        var cx = x
        var cy = y
        var count = 0
        for (ch in value) {
            if (count++ >= maxChars || cy + 16 * scale > logicalHeight) break
            if (ch == '\n') { cx = x; cy += 18 * scale; continue }
            val code = ch.code.coerceIn(32, 126) - 32
            for (row in 0 until 16) {
                val bits = font8x16[code * 16 + row]
                for (col in 0 until 8) if ((bits and (1 shl col)) != 0) {
                    rect(cx + col * scale, cy + row * scale, scale, scale, color)
                }
            }
            cx += 9 * scale
            if (cx + 8 * scale >= logicalWidth) break
        }
    }

    private fun icon(x: Int, y: Int, color: Int, letter: String, label: String) {
        rect(x + 8, y, 44, 44, color)
        rect(x + 11, y + 3, 38, 38, rgb(25, 39, 61))
        text(x + 25, y + 12, letter, rgb(230, 242, 255))
        text(x - 5, y + 50, label, rgb(218, 231, 247), maxChars = 10)
    }

    private fun drawBase() {
        // Deep blue gradient wallpaper, clipped by actual framebuffer geometry and pitch.
        for (y in 0 until logicalHeight) {
            val t = y * 72 / logicalHeight
            val c = rgb(8 + t / 5, 20 + t / 3, 42 + t)
            rect(0, y, logicalWidth, 1, c)
        }
        // Soft geometric light bands give the empty desktop a little depth.
        rect(0, logicalHeight * 2 / 3, logicalWidth, logicalHeight / 3, rgb(15, 31, 56))
        val cyan = rgb(76, 207, 224)
        val white = rgb(232, 241, 251)
        val muted = rgb(155, 176, 201)
        val panel = rgb(21, 34, 53)
        val panel2 = rgb(28, 44, 66)
        val border = rgb(51, 70, 96)

        // Top status bar.
        rect(0, 0, logicalWidth, 34, rgb(12, 22, 37))
        rect(0, 33, logicalWidth, 1, border)
        text(18, 9, "KRAOS", cyan)
        text(96, 9, "Desktop", white)
        text((logicalWidth - 18 * 9).coerceAtLeast(16), 9, "Kotlin Native OS", muted, maxChars = 18)

        // Desktop shortcuts.
        icon(26, 76, rgb(62, 156, 223), "T", "Terminal")
        icon(26, 160, rgb(67, 177, 132), "F", "Files")
        icon(26, 244, rgb(222, 178, 76), "N", "Notes")
        icon(26, 328, rgb(130, 145, 170), "S", "Settings")

        // Welcome window, centered and bounded for small supported resolutions.
        val ww = (logicalWidth - 176).coerceIn(420, 700)
        val wh = (logicalHeight - 206).coerceIn(300, 440)
        val wx = ((logicalWidth - ww) / 2).coerceAtLeast(100)
        val wy = ((logicalHeight - wh) / 2).coerceAtLeast(54)
        rect(wx + 5, wy + 7, ww, wh, rgb(4, 10, 18)) // simple drop shadow
        rect(wx, wy, ww, wh, panel)
        rect(wx, wy, ww, 38, panel2)
        rect(wx, wy + 37, ww, 1, border)
        val appName = when (activeApp) {
            1 -> "Terminal"
            2 -> "Files"
            3 -> "Notes"
            4 -> "Settings"
            5 -> "About"
            else -> "Welcome to KraOS"
        }
        text(wx + 16, wy + 11, appName, white)
        // Window controls.
        rect(wx + ww - 72, wy + 12, 10, 10, rgb(95, 116, 142))
        rect(wx + ww - 48, wy + 12, 10, 10, rgb(95, 116, 142))
        rect(wx + ww - 24, wy + 12, 10, 10, rgb(220, 96, 102))

        // Application sidebar and landing content.
        rect(wx, wy + 38, 150, wh - 38, rgb(17, 29, 46))
        text(wx + 16, wy + 62, "WORKSPACE", muted, maxChars = 14)
        rect(wx + 10, wy + 88, 130, 30, rgb(38, 61, 83))
        rect(wx + 10, wy + 88, 3, 30, cyan)
        text(wx + 23, wy + 96, "Overview", white)
        text(wx + 23, wy + 137, "Applications", muted)
        text(wx + 23, wy + 173, "System info", muted)

        val cx = wx + 178
        text(cx, wy + 66, "Good to see you.", white, scale = 2, maxChars = 25)
        text(cx, wy + 111, "Press 1-5 to open apps; Esc returns here.", muted, maxChars = 40)
        rect(cx, wy + 146, ww - 204, 1, border)
        text(cx, wy + 168, "GETTING STARTED", cyan, maxChars = 22)

        val cardY = wy + 198
        val cardW = (ww - 204 - 24) / 2
        rect(cx, cardY, cardW, 76, panel2)
        rect(cx + cardW + 12, cardY, cardW, 76, panel2)
        rect(cx + 14, cardY + 14, 28, 28, rgb(38, 93, 128))
        text(cx + 23, cardY + 20, "T", white)
        text(cx + 52, cardY + 14, "Terminal", white)
        text(cx + 52, cardY + 39, "Run built-in commands", muted, maxChars = 22)
        rect(cx + cardW + 26, cardY + 14, 28, 28, rgb(45, 113, 87))
        text(cx + cardW + 35, cardY + 20, "N", white)
        text(cx + cardW + 64, cardY + 14, "Notes", white)
        text(cx + cardW + 64, cardY + 39, "Keep a local note", muted, maxChars = 20)
        text(cx, wy + wh - 48, "SYSTEM READY | 32-bit RGB framebuffer", muted, maxChars = 38)

        // Bottom dock / taskbar.
        val dockW = 360
        val dockX = (logicalWidth - dockW) / 2
        val dockY = logicalHeight - 66
        rect(dockX, dockY, dockW, 50, rgb(12, 22, 37))
        rect(dockX, dockY, dockW, 1, border)
        val dockColors = intArrayOf(rgb(62, 156, 223), rgb(67, 177, 132), rgb(222, 178, 76), rgb(130, 145, 170), rgb(163, 112, 211))
        val letters = arrayOf("T", "F", "N", "S", "A")
        for (i in 0 until 5) {
            val x = dockX + 22 + i * 64
            rect(x, dockY + 8, 34, 34, dockColors[i])
            text(x + 12, dockY + 17, letters[i], white)
        }
        text((logicalWidth - 17 * 9).coerceAtLeast(8), fb.height - 57, "KraOS", white, maxChars = 8)
        if (activeApp != 0) {
            rect(cx, wy + 168, ww - 204, 180, panel)
            text(cx, wy + 178, appName.uppercase(), cyan, maxChars = 24)
            when (activeApp) {
                1 -> {
                    text(cx, wy + 207, "KraOS terminal", white)
                    text(cx, wy + 234, "Type help in serial console for commands.", muted, maxChars = 40)
                    text(cx, wy + 270, "$  _", cyan)
                }
                2 -> {
                    text(cx, wy + 207, "RAM files", white)
                    text(cx, wy + 234, "Documents     folder", muted)
                    text(cx, wy + 260, "README.txt    1.2 KB", muted)
                    text(cx, wy + 286, "notes.txt     0 bytes", muted)
                }
                3 -> {
                    text(cx, wy + 207, "Quick note", white)
                    rect(cx, wy + 232, ww - 218, 110, rgb(17, 29, 46))
                    text(cx + 10, wy + 244, "Notes are stored in RAM for this session.", muted, maxChars = 39)
                    text(cx + 10, wy + 274, "Text editor coming soon.", muted)
                }
                4 -> {
                    text(cx, wy + 207, "Appearance", white)
                    text(cx, wy + 235, "Theme              Midnight", muted)
                    text(cx, wy + 263, "Resolution         ${fb.width} x ${fb.height}", muted)
                    text(cx, wy + 291, "Renderer           Software RGB", muted)
                }
                5 -> {
                    text(cx, wy + 207, "KraOS Kotlin/Native", white)
                    text(cx, wy + 235, "A freestanding x86_64 operating system.", muted, maxChars = 40)
                    text(cx, wy + 263, "Built in Kotlin. Running on bare metal.", muted, maxChars = 40)
                }
            }
        }
    }

    fun render(framebuffer: Framebuffer) {
        fb = framebuffer
        stride = fb.pitch / 4
        pixels = intPtr(fb.base)
        drawBase()
    }

    fun selectApp(index: Int): Boolean {
        if (activeApp == index.coerceIn(0, 5)) return false
        activeApp = index.coerceIn(0, 5)
        drawBase()
        return true
    }
}
