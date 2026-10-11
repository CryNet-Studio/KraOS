@file:OptIn(kotlinx.cinterop.ExperimentalForeignApi::class, kotlin.experimental.ExperimentalNativeApi::class)

package kraos.desktop

import kraos.boot.Framebuffer
import kraos.platform.intPtr
import kotlinx.cinterop.get
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
    private var noteText = ""
    private var selectedFile = 0
    private var terminalInput = ""
    private var terminalOutput = "KraOS shell ready\nType help for commands"
    private var cursorX = 0
    private var cursorY = 0
    private var cursorLeft = 0
    private var cursorTop = 0
    private var cursorDrawn = false
    private val cursorSaved = IntArray(32 * 40)

    private fun rgb(r: Int, g: Int, b: Int): Int =
        (r shl fb.redPosition) or (g shl fb.greenPosition) or (b shl fb.bluePosition)

    private fun physicalPixel(x: Int, y: Int, color: Int) {
        if (x in 0 until fb.width && y in 0 until fb.height) pixels[y * stride + x] = color
    }

    private fun cursorInside(x: Int, y: Int): Boolean =
        (y < 16 && x <= minOf(9, y / 2)) || (y in 11..18 && x in 4..8)

    private fun restoreCursor() {
        if (!cursorDrawn) return
        val width = 16 * uiScale
        val height = 20 * uiScale
        val left = cursorLeft
        val top = cursorTop
        for (y in 0 until height) for (x in 0 until width) {
            physicalPixel(left + x, top + y, cursorSaved[y * width + x])
        }
        cursorDrawn = false
    }

    private fun drawCursor() {
        val width = 16 * uiScale
        val height = 20 * uiScale
        val left = (cursorX * uiScale).coerceIn(0, (fb.width - width).coerceAtLeast(0))
        val top = (cursorY * uiScale).coerceIn(0, (fb.height - height).coerceAtLeast(0))
        cursorLeft = left
        cursorTop = top
        for (y in 0 until height) for (x in 0 until width) {
            cursorSaved[y * width + x] = if (left + x < fb.width && top + y < fb.height)
                pixels[(top + y) * stride + left + x] else 0
        }
        val edge = rgb(4, 10, 18)
        val fill = rgb(245, 249, 255)
        for (y in 0 until height) for (x in 0 until width) {
            val gx = x / uiScale
            val gy = y / uiScale
            if (!cursorInside(gx, gy)) continue
            val border = !cursorInside(gx - 1, gy) || !cursorInside(gx + 1, gy) ||
                !cursorInside(gx, gy - 1) || !cursorInside(gx, gy + 1)
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

    private fun blendPixel(x: Int, y: Int, color: Int, alpha: Int) {
        if (alpha <= 0 || x !in 0 until fb.width || y !in 0 until fb.height) return
        val index = y * stride + x
        val background = pixels[index]
        val inverse = 15 - alpha
        val red = (((background ushr fb.redPosition) and 255) * inverse + ((color ushr fb.redPosition) and 255) * alpha + 7) / 15
        val green = (((background ushr fb.greenPosition) and 255) * inverse + ((color ushr fb.greenPosition) and 255) * alpha + 7) / 15
        val blue = (((background ushr fb.bluePosition) and 255) * inverse + ((color ushr fb.bluePosition) and 255) * alpha + 7) / 15
        pixels[index] = (red shl fb.redPosition) or (green shl fb.greenPosition) or (blue shl fb.bluePosition)
    }

    private fun text(x: Int, y: Int, value: String, color: Int, scale: Int = 1, maxChars: Int = 128) {
        var cx = x
        var cy = y
        var count = 0
        for (ch in value) {
            if (count++ >= maxChars || cy + 16 * scale > logicalHeight) break
            if (ch == '\n') { cx = x; cy += 18 * scale; continue }
            val code = ch.code.coerceIn(32, 126) - 32
            if (uiScale == 2) {
                val originX = cx * uiScale
                val originY = cy * uiScale
                for (row in 0 until 32) for (col in 0 until 16) {
                    val alpha = smoothFont16x32[code * 512 + row * 16 + col].toInt() and 255
                    if (alpha == 0) continue
                    for (sy in 0 until scale) for (sx in 0 until scale) {
                        blendPixel(originX + col * scale + sx, originY + row * scale + sy, color, alpha)
                    }
                }
            } else {
                for (row in 0 until 16) {
                    val bits = font8x16[code * 16 + row]
                    for (col in 0 until 8) if ((bits and (1 shl col)) != 0) {
                        rect(cx + col * scale, cy + row * scale, scale, scale, color)
                    }
                }
            }
            cx += 8 * scale
            if (cx + 8 * scale >= logicalWidth) break
        }
    }

    private fun drawNoteContent(cx: Int, wy: Int, ww: Int, wh: Int, muted: Int, white: Int) {
        val fieldHeight = (wh - 272).coerceAtLeast(42)
        rect(cx, wy + 232, ww - 228, fieldHeight, rgb(17, 29, 46))
        text(
            cx + 10,
            wy + 240,
            if (noteText.isEmpty()) "Type a note here..." else noteText,
            if (noteText.isEmpty()) muted else white,
            maxChars = 160
        )
    }

    private fun drawFilesContent(cx: Int, wy: Int, ww: Int, muted: Int, white: Int, cyan: Int) {
        val contentWidth = ww - 204
        val listWidth = (contentWidth / 3).coerceAtLeast(72)
        val previewX = cx + listWidth + 10
        val previewWidth = contentWidth - listWidth - 10
        val itemY = intArrayOf(wy + 204, wy + 238)
        val names = arrayOf("README.txt", "notes.txt")
        for (i in 0..1) {
            rect(cx, itemY[i], listWidth, 28, if (selectedFile == i) rgb(38, 61, 83) else rgb(17, 29, 46))
            if (selectedFile == i) rect(cx, itemY[i], 3, 28, cyan)
            text(cx + 10, itemY[i] + 7, names[i], if (selectedFile == i) white else muted, maxChars = 18)
        }
        rect(previewX, wy + 204, previewWidth, 76, rgb(17, 29, 46))
        text(previewX + 8, wy + 211, names[selectedFile], cyan, maxChars = 18)
        val preview = if (selectedFile == 0) {
            "KraOS bare-metal OS\nKotlin/Native x86_64"
        } else if (noteText.isEmpty()) {
            "(empty · RAM only)"
        } else {
            noteText
        }
        text(previewX + 8, wy + 234, preview, white, maxChars = 36)
    }

    private fun drawTerminalContent(cx: Int, wy: Int, ww: Int, wh: Int, white: Int, cyan: Int) {
        rect(cx, wy + 200, ww - 204, wh - 242, rgb(17, 29, 46))
        if (terminalOutput.isNotEmpty()) text(cx + 10, wy + 208, terminalOutput, white, maxChars = 120)
        text(cx + 10, wy + 270, "kraos> ${terminalInput}_", cyan, maxChars = 50)
    }

    private fun executeTerminalCommand() {
        val command = terminalInput.trim().lowercase()
        terminalOutput = when {
            command.isEmpty() -> ""
            command == "help" -> "help  ls  cat README.txt\ncat notes.txt  about  clear"
            command == "ls" || command == "dir" -> "README.txt\nnotes.txt"
            command == "cat readme.txt" -> "KraOS x86_64\nKotlin/Native desktop"
            command == "cat notes.txt" -> if (noteText.isEmpty()) "(notes.txt is empty)" else noteText
            command == "about" -> "KraOS early desktop\nKotlin/Native · x86_64"
            command == "clear" -> ""
            else -> "command not found: ${command.take(36)}"
        }
        if (terminalOutput.length > 120) terminalOutput = terminalOutput.take(120)
        terminalInput = ""
    }

    private fun icon(x: Int, y: Int, color: Int, letter: String, label: String) {
        rect(x + 8, y, 44, 44, color)
        rect(x + 11, y + 3, 38, 38, rgb(25, 39, 61))
        text(x + 25, y + 12, letter, rgb(230, 242, 255))
        text(x - 5, y + 50, label, rgb(218, 231, 247), maxChars = 10)
    }

    private fun drawBase() {
        restoreCursor()
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
        text((logicalWidth - 18 * 8).coerceAtLeast(16), 9, "Kotlin Native OS", muted, maxChars = 18)

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
        val selectedLabel = if (activeApp == 0) "Overview" else appName
        rect(wx + 10, wy + 88, 130, 30, rgb(38, 61, 83))
        rect(wx + 10, wy + 88, 3, 30, cyan)
        text(wx + 23, wy + 96, selectedLabel, white, maxChars = 14)
        text(wx + 23, wy + 137, if (activeApp == 0) "Applications" else "Overview", muted)
        text(wx + 23, wy + 173, "System info", muted)

        val cx = wx + 178
        val heading = when (activeApp) {
            1 -> "Terminal"
            2 -> "File browser"
            3 -> "Notes"
            4 -> "Appearance"
            5 -> "About KraOS"
            else -> "Good to see you."
        }
        val subtitle = when (activeApp) {
            1 -> "A command shell for the KraOS system."
            2 -> "Browse the in-memory workspace."
            3 -> "A scratchpad for this boot session."
            4 -> "Display and theme preferences."
            5 -> "KraOS | x86_64 | Kotlin/Native"
            else -> "Press 1-5 to open apps; Esc returns here."
        }
        text(cx, wy + 66, heading, white, scale = 2, maxChars = 25)
        text(cx, wy + 111, subtitle, muted, maxChars = 40)
        rect(cx, wy + 146, ww - 204, 1, border)

        if (activeApp == 0) {
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
        } else {
            rect(cx, wy + 168, ww - 204, wh - 202, panel)
            text(cx, wy + 178, appName.uppercase(), cyan, maxChars = 24)
            when (activeApp) {
                1 -> {
                    drawTerminalContent(cx, wy, ww, wh, white, cyan)
                }
                2 -> {
                    drawFilesContent(cx, wy, ww, muted, white, cyan)
                }
                3 -> {
                    text(cx, wy + 207, "Quick note · RAM only", white)
                    drawNoteContent(cx, wy, ww, wh, muted, white)
                }
                4 -> {
                    text(cx, wy + 207, "Theme              Midnight", muted)
                    text(cx, wy + 234, "Resolution         ${fb.width} x ${fb.height}", muted)
                    text(cx, wy + 261, "Renderer           Software RGB", muted)
                }
                5 -> {
                    text(cx, wy + 207, "A freestanding x86_64 operating system.", white, maxChars = 40)
                    text(cx, wy + 234, "Built in Kotlin. Running on bare metal.", muted, maxChars = 40)
                }
            }
        }
        text(cx, wy + wh - 28, "SYSTEM READY | ${fb.width} x ${fb.height}", muted, maxChars = 38)

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
        text((logicalWidth - 17 * 8).coerceAtLeast(8), logicalHeight - 57, "KraOS", white, maxChars = 8)
        drawCursor()
    }

    val inputWidth: Int get() = logicalWidth
    val inputHeight: Int get() = logicalHeight
    val foregroundApp: Int get() = activeApp

    /** Edit the shell command line and execute the supported in-memory commands. */
    fun handleTerminalKey(key: Int): Boolean {
        if (activeApp != 1) return false
        when (key) {
            8 -> if (terminalInput.isNotEmpty()) terminalInput = terminalInput.dropLast(1) else return false
            10 -> executeTerminalCommand()
            in 32..126 -> {
                if (terminalInput.length >= 40) return false
                terminalInput += key.toChar()
            }
            else -> return false
        }
        restoreCursor()
        val ww = (logicalWidth - 176).coerceIn(420, 700)
        val wh = (logicalHeight - 206).coerceIn(300, 440)
        val wx = ((logicalWidth - ww) / 2).coerceAtLeast(100)
        val wy = ((logicalHeight - wh) / 2).coerceAtLeast(54)
        drawTerminalContent(wx + 178, wy, ww, wh, rgb(232, 241, 251), rgb(88, 214, 224))
        drawCursor()
        return true
    }

    /** Append ASCII, Enter, or Backspace to the bounded boot-session Notes buffer. */
    fun handleNoteKey(key: Int): Boolean {
        if (activeApp != 3) return false
        when (key) {
            8 -> {
                if (noteText.isEmpty()) return false
                noteText = noteText.dropLast(1)
            }
            10 -> {
                if (noteText.count { it == '\n' } >= 2 || noteText.length >= 156) return false
                noteText += '\n'
            }
            in 32..126 -> {
                if (noteText.length >= 156) return false
                val ww = (logicalWidth - 176).coerceIn(420, 700)
                val lineLimit = ((ww - 248) / 9).coerceIn(12, 50)
                val lineLength = noteText.length - (noteText.lastIndexOf('\n') + 1)
                if (lineLength >= lineLimit) {
                    if (noteText.count { it == '\n' } >= 2) return false
                    noteText += '\n'
                }
                noteText += key.toChar()
            }
            else -> return false
        }
        restoreCursor()
        val ww = (logicalWidth - 176).coerceIn(420, 700)
        val wh = (logicalHeight - 206).coerceIn(300, 440)
        val wx = ((logicalWidth - ww) / 2).coerceAtLeast(100)
        val wy = ((logicalHeight - wh) / 2).coerceAtLeast(54)
        drawNoteContent(wx + 178, wy, ww, wh, rgb(155, 176, 201), rgb(232, 241, 251))
        drawCursor()
        return true
    }

    fun render(framebuffer: Framebuffer) {
        fb = framebuffer
        uiScale = if (fb.width >= 1600) 2 else 1
        stride = fb.pitch / 4
        pixels = intPtr(fb.base)
        cursorX = logicalWidth / 2
        cursorY = logicalHeight / 2
        Input.initialize(logicalWidth, logicalHeight)
        drawBase()
    }

    fun movePointer(x: Int, y: Int): Boolean {
        val nextX = x.coerceIn(0, logicalWidth - 1)
        val nextY = y.coerceIn(0, logicalHeight - 1)
        if (nextX == cursorX && nextY == cursorY) return false
        restoreCursor()
        cursorX = nextX
        cursorY = nextY
        drawCursor()
        return true
    }

    fun click(x: Int, y: Int): Int? {
        val dockWidth = 360
        val dockX = (logicalWidth - dockWidth) / 2
        val dockY = logicalHeight - 66
        if (y in (dockY + 8)..(dockY + 42)) {
            for (i in 0 until 5) {
                val iconX = dockX + 22 + i * 64
                if (x in iconX..(iconX + 34)) return if (selectApp(i + 1)) i + 1 else null
            }
        }
        val shortcut = when (y) {
            in 76..140 -> 1
            in 160..224 -> 2
            in 244..308 -> 3
            in 328..392 -> 4
            else -> 0
        }
        if (x in 0..100 && shortcut != 0) return if (selectApp(shortcut)) shortcut else null

        val ww = (logicalWidth - 176).coerceIn(420, 700)
        val wh = (logicalHeight - 206).coerceIn(300, 440)
        val wx = ((logicalWidth - ww) / 2).coerceAtLeast(100)
        val wy = ((logicalHeight - wh) / 2).coerceAtLeast(54)
        if (activeApp == 2) {
            val fileX = wx + 178
            val listWidth = ((ww - 204) / 3).coerceAtLeast(72)
            val selected = when {
                x in fileX..(fileX + listWidth) && y in (wy + 204)..(wy + 232) -> 0
                x in fileX..(fileX + listWidth) && y in (wy + 238)..(wy + 266) -> 1
                else -> -1
            }
            if (selected >= 0) {
                if (selectedFile != selected) {
                    selectedFile = selected
                    drawBase()
                }
                return null
            }
        }
        if (x in (wx + 10)..(wx + 140)) {
            if (y in (wy + 129)..(wy + 154) && activeApp != 0) {
                return if (selectApp(0)) 0 else null
            }
            if (y in (wy + 160)..(wy + 186)) return if (selectApp(5)) 5 else null
        }
        if (x in (wx + ww - 78)..(wx + ww - 42) && y in (wy + 8)..(wy + 28)) {
            return if (selectApp(0)) 0 else null
        }
        return null
    }

    fun selectApp(index: Int): Boolean {
        if (activeApp == index.coerceIn(0, 5)) return false
        activeApp = index.coerceIn(0, 5)
        drawBase()
        return true
    }
}
