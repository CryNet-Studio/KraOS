@file:OptIn(kotlinx.cinterop.ExperimentalForeignApi::class, kotlin.experimental.ExperimentalNativeApi::class)

/*
 * KraOS 内核入口。
 *
 * 这一层跑在 Kotlin/Native 里 —— 上面的 BootHeap / Serial / BootParser 都已经是
 * 真正的 Kotlin 代码，而不是 C。启动桩（boot/entry.S）只负责 CPU 级别的建长模式，
 * 之后就再也没出现过。
 *
 * 刻意不带 package：Kotlin/Native 要求 `main` 位于根包。
 */

import kraos.boot.BootHeap
import kraos.boot.LibcShims
import kraos.boot.BootInfo
import kraos.boot.MEMORY_AVAILABLE
import kraos.boot.MULTIBOOT2_BOOTLOADER_MAGIC
import kraos.boot.kraos_arena_end
import kraos.boot.kraos_cpu_count
import kraos.boot.kraos_arena_start
import kraos.boot.kraos_mb_info
import kraos.boot.kraos_mb_magic
import kraos.boot.parseBootInfo
import kraos.console.haltForever
import kraos.console.write
import kraos.console.writeHex
import kraos.console.writeLine
import kraos.console.writeNumber
import kraos.kernel.PAGE_SIZE
import kraos.kernel.PhysicalMemory
import kraos.desktop.Desktop
import kraos.desktop.Keyboard
import kraos.desktop.Input
import kraos.desktop.Keyboard.desktopApp
import kraos.platform.writeU64

/** 引导信息；main() 跑完之后仍然可查。 */
private var bootInfo: BootInfo? = null

/** 供内核其余部分读取引导信息。 */
fun kraosBootInfo(): BootInfo? = bootInfo

fun main() {
    writeLine("[kraos] ===== KraOS 内核启动 =====")

    // BootHeap 采用懒初始化（第一次分配时兜底），这里不需要显式调用 ——
    // Konan_main 在进入本函数之前，Kotlin_initRuntimeIfNeeded() 就已经
    // 通过 mmap 向它要过内存了。

    val magic = kraos_mb_magic()
    write("[kraos] multiboot2 magic = ")
    writeHex(magic)
    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
        writeLine("  <- 不是有效的 multiboot2 引导器")
    } else {
        writeLine("  ✓")
    }

    val info = parseBootInfo(kraos_mb_info())
    if (info == null) {
        writeLine("[kraos] FATAL: 无法解析 multiboot2 信息结构体")
        haltForever()
        return
    }
    bootInfo = info

    val regions = info.memoryRegions
    write("[kraos] 内存区数量 = ${regions.size}")
    writeLine("")
    for (r in regions) {
        write("        base=")
        writeHex(r.base)
        write(" len=")
        writeHex(r.length)
        write(" type=${r.type}")
        writeLine(if (r.type == MEMORY_AVAILABLE) " (可用)" else "")
    }

    var totalAvailable = 0L
    for (r in regions) if (r.type == MEMORY_AVAILABLE) totalAvailable += r.length

    val fb = info.framebuffer
    if (fb != null) {
        write("[kraos] framebuffer = ${fb.width}x${fb.height}@${fb.bpp}bpp base=")
        writeHex(fb.base)
        writeLine("")
    } else {
        writeLine("[kraos] framebuffer = 无（纯串口模式）")
    }

    // 垫片自检：既验证 brk/mmap 改道通了，也把那几个 @CName 实现锚定下来
    // （Kotlin 侧不引用的话，-opt 会把它们当死代码删掉）。
    val shim = LibcShims.selfCheck()
    write("[kraos] libc 垫片 brk=")
    writeHex(shim.breakValue)
    write(" mmap 探针=")
    writeHex(shim.mmapProbe)
    writeLine("")

    write("[kraos] 引导堆已分配 ${BootHeap.allocations} 次 / ")
    writeNumber(BootHeap.bytesAllocated shr 10)
    writeLine(" KiB")

    // 到这里，Kotlin 代码已经在裸机上真正执行了，而且它跑在 K/N 运行时之上。
    // 后续阶段（P2 页管理器、P3 调度器）从这里往下长。
    writeLine("[kraos] Kotlin/Native 内核入口就绪；P1 引导骨架完成")

    // P2：基于 multiboot2 内存图的物理页帧分配器。
    //
    // 需要预先占住的区间是「内核自己已经踩着的」：内核映像、页表、引导栈，
    // 以及 boot/libc_shim.c 里那块 64 MiB 的 .bss 竞技场（kraos_heap）。
    // A2 把内核搬进高半区、竞技场换成真实页之后，这个列表会缩短。
    val kernelEnd = kraos_arena_end()
    PhysicalMemory.init(
        info,
        reserveRanges = listOf(0L to kernelEnd),
    )
    write("[kraos] ")
    writeLine(PhysicalMemory.stats())

    // 真跑一遍分配-释放，确认位图逻辑在真机上成立（而不是只看它能编过）。
    // 判据刻意只选「逻辑上必然成立」的性质：不能是地址，也不能落在内核自己
    // 占着的区间里。单帧紧贴空闲区首帧、64 帧紧贴首帧之后，这类邻接关系依赖
    // 内存图的空洞分布，不该拿来当判据。
    val reservedEnd = kraos_arena_end()
    val probeA = PhysicalMemory.allocFrame()
    val probeB = PhysicalMemory.allocFrames(64)
    val probeC = PhysicalMemory.allocFrame()
    val aligned = probeA % PAGE_SIZE == 0L && probeB % PAGE_SIZE == 0L && probeC % PAGE_SIZE == 0L
    val distinct = probeA != probeB && probeB != probeC && probeA != probeC
    val outside = probeA >= reservedEnd && probeB >= reservedEnd && probeC >= reservedEnd
    val marked = PhysicalMemory.isUsed(probeA) && PhysicalMemory.isUsed(probeC)
    PhysicalMemory.freeFrames(probeA, 1)
    PhysicalMemory.freeFrames(probeB, 64)
    PhysicalMemory.freeFrames(probeC, 1)
    val reclaimed = !PhysicalMemory.isUsed(probeA) && !PhysicalMemory.isUsed(probeC)
    write("[kraos] 页分配自检: 首帧=")
    writeHex(probeA)
    write(" 64帧=")
    writeHex(probeB)
    write(" 末帧=")
    writeHex(probeC)
    writeLine("")
    write("[kraos] 页分配判定: ")
    write("页对齐=")
    write(if (aligned) "✓" else "✗")
    write(" 互不相同=")
    write(if (distinct) "✓" else "✗")
    write(" 避开内核区=")
    write(if (outside) "✓" else "✗")
    write(" 标记已用=")
    write(if (marked) "✓" else "✗")
    write(" 释放归还=")
    write(if (reclaimed) "✓" else "✗")
    writeLine("")

    // P1 的验收横幅：固定串 + 内存大小 + CPU 核数，便于脚本 grep。
    writeLine("")
    write("KraOS boot ok  ")
    write("mem=")
    writeNumber(totalAvailable shr 20)
    write(" MiB  ")
    write("cpu=")
    writeNumber(kraos_cpu_count())
    writeLine("")

    val desktopFramebuffer = info.framebuffer
    if (desktopFramebuffer != null) {
        Desktop.render(desktopFramebuffer)
        writeLine("[desktop] Kotlin framebuffer desktop rendered")
    } else {
        writeLine("[desktop] 无有效 RGB framebuffer，继续纯串口模式")
    }

    if (desktopFramebuffer != null) {
        writeLine(if (Input.mouseReady) "[input] PS/2 mouse ready" else "[input] PS/2 mouse unavailable; keyboard remains active")
        writeLine("[desktop] mouse: click dock/shortcuts; keyboard: 1-5 apps, Esc Overview")
        var oldX = Input.mouseX
        var oldY = Input.mouseY
        var oldButtons = Input.buttons
        while (true) {
            Input.poll(Desktop.inputWidth, Desktop.inputHeight)
            if (Input.mouseX != oldX || Input.mouseY != oldY) {
                Desktop.movePointer(Input.mouseX, Input.mouseY)
                oldX = Input.mouseX
                oldY = Input.mouseY
            }
            val leftPressed = (Input.buttons and 1) != 0 && (oldButtons and 1) == 0
            if (leftPressed) {
                val clickedApp = Desktop.click(Input.mouseX, Input.mouseY)
                if (clickedApp != null) {
                    write("[desktop] mouse opened app = ")
                    writeNumber(clickedApp.toLong())
                    writeLine("")
                }
            }
            oldButtons = Input.buttons

            val key = Keyboard.pollKey()
            if (key >= 0) {
                if (key != 27 && Desktop.foregroundApp == 3) {
                    Desktop.handleNoteKey(key)
                } else if (key != 27 && Desktop.foregroundApp == 1) {
                    Desktop.handleTerminalKey(key)
                } else {
                    val app = desktopApp(key)
                    if (app != null && Desktop.selectApp(app)) {
                        write("[desktop] keyboard opened app = ")
                        writeNumber(app.toLong())
                        writeLine("")
                    }
                }
            }
        }
    }

    writeLine("[kraos] 内核 main 返回，停机")
    haltForever()
}