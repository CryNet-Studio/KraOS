package kraos.kernel

import kraos.boot.BootInfo
import kraos.boot.MEMORY_AVAILABLE
import kraos.platform.zeroMemory

/*
 * 物理页帧分配器。
 *
 * P2 的第一块砖。此前内核里所有的内存都来自 boot/libc_shim.c 里手工划的
 * 64 MiB `.bss` 竞技场：一个 bump 指针，只增不减，也不看 multiboot2 说了什么。
 * 那块竞技场的问题是它既不知道内存图、也不知道自己占了多少，出问题只能靠猜。
 *
 * 这里改成正经的分配器：
 *
 *   1. 输入是 `BootInfo.memoryRegions`（Kotlin 自己解析的，见 BootParser.kt），
 *      只认 `MEMORY_AVAILABLE` 类型的区间；
 *   2. 用位图逐页标记，一个 bit 一个 4 KiB 页帧；
 *   3. [reserve] 把内核自己占着的东西（内核映像、页表、引导栈、那块旧竞技场）
 *      先标掉，免得被分配出去把自己踩了；
 *   4. [allocFrames] 扫连续空闲位，[freeFrames] 放回去。
 *
 * 全程是纯逻辑 + 一次 `zeroMemory`，没有任何特权指令，所以放在 Kotlin 里完全合理 ——
 * 这也正是 P2 想证明的事：内核的绝大部分代码不必是 C。
 *
 * ## 关于地址范围
 *
 * 引导桩（boot/entry.S）用 2 MiB 大页把 [0, 4 GiB) 整段线性映射了，所以这里的
 * 「物理地址」在 4 GiB 以下可以直接当指针用，不用先建映射。
 *
 * 但 **4 GiB 以上的内存此刻是够不着的**，multiboot2 报的那块
 * `0xfd00000000..+0x300000000` 不在本分配器管辖范围内 —— 等 A2 把内核搬进
 * 高半区、用真正的页表管理内存之后再把它纳进来。
 */

/** 页大小。x86-64 的自然对齐页就是 4 KiB。 */
const val PAGE_SIZE: Long = 4096

/** 本分配器管辖区间的下界（含）。 */
const val MANAGED_BASE: Long = 0

private const val BITS_PER_WORD = 64

/**
 * 物理页帧分配器。
 *
 * 用法上刻意做成 `object` 而不是可实例化类：它是全内核唯一的一份状态，
 * 做成单例可以避免「谁持有了一份过期副本」这种问题。
 */
object PhysicalMemory {

    /** 位图；null 表示还没 [init]。 */
    private var bitmap: LongArray? = null

    /** 管辖区间的物理上界（不含）。 */
    private var managedEnd: Long = 0

    /** 总页帧数。 */
    var totalFrames: Int = 0
        private set

    /** 已用页帧数（含被 [reserve] 标掉的）。 */
    var usedFrames: Int = 0
        private set

    /** 已分配字节数，按页向上取整。 */
    val usedBytes: Long get() = usedFrames.toLong() * PAGE_SIZE

    val initialized: Boolean get() = bitmap != null

    /** 可分配的总字节数。 */
    val totalBytes: Long get() = totalFrames.toLong() * PAGE_SIZE

    /**
     * 依据 multiboot2 内存图建立位图。
     *
     * @param info 引导信息；必须已经通过 `parseBootInfo` 解析
     * @param reserveRanges 需要预先占用的 `(起, 止)` 物理地址区间，含起不含止
     * @throws IllegalStateException 重复初始化
     * @throws IllegalArgumentException 内存图里找不到任何可用内存
     */
    fun init(info: BootInfo, reserveRanges: List<Pair<Long, Long>> = emptyList()) {
        check(bitmap == null) { "PhysicalMemory 已经初始化过了" }

        // 先量出管辖区间：所有「可用」区间与 [MANAGED_BASE, MANAGED_BASE) 求并，
        // 忽略 4 GiB 以上的部分（此刻没有映射，见文件头的说明）。
        var lo = Long.MAX_VALUE
        var hi = 0L
        for (r in info.memoryRegions) {
            if (r.type != MEMORY_AVAILABLE) continue
            val base = maxOf(r.base, MANAGED_BASE)
            val end = minOf(r.base + r.length, FOUR_GIB)
            if (end <= base) continue
            if (base < lo) lo = base
            if (end > hi) hi = end
        }
        check(lo < hi) { "multiboot2 内存图里没有 4 GiB 以下的可用内存" }

        // 区间可能不连续（中间夹着保留段），所以位图按「从 lo 到 hi 的全部页帧」
        // 来建 —— 中间那些不属实的页帧最后会被 [markRange] 标成已用。
        totalFrames = ((hi - lo) / PAGE_SIZE).toInt()
        managedEnd = hi

        val words = (totalFrames + BITS_PER_WORD - 1) / BITS_PER_WORD
        val words64 = LongArray(words)
        bitmap = words64

        // 先全标成「已用」，再把真正可用的区间清出来。
        words64.fill(-1L)
        for (r in info.memoryRegions) {
            if (r.type != MEMORY_AVAILABLE) continue
            val base = maxOf(r.base, lo)
            val end = minOf(r.base + r.length, hi)
            if (end <= base) continue
            markRange(base, end, free = true)
        }

        // 内核自己占着的最后再压回去 —— 必须在可用区间清完之后做。
        for ((start, end) in reserveRanges) reserve(start, end)
    }

    /**
     * 把 `[start, end)` 标成已用。
     *
     * 落在管辖区间之外的部分直接忽略：引导栈之类的东西虽然也在这个区间里，
     * 但它本来就该由内核自己保证不被分配器交出去。
     */
    fun reserve(start: Long, end: Long) {
        markRange(start, end, free = false)
    }

    /**
     * 分配 [count] 个**连续**页帧，返回首个页帧的物理地址。
     *
     * 策略很简单：从头往后找第一个空闲位，看从这里起能不能连着数出 [count] 个
     * 空闲位；不行就从下一个位继续。分配请求本来就不密集（K/N 启动期总共几十次），
     * 这点 O(帧数 × count) 的扫描代价可以忽略，换来的是一眼能验证的正确性 ——
     * 早先那个版本试图跨字做位段掩码，既难读又真的写错了。
     *
     * @return 物理地址；内存不足时返回 0
     */
    fun allocFrames(count: Int): Long {
        if (count <= 0) return 0
        val bm = bitmap ?: return 0

        var at = 0
        while (at + count <= totalFrames) {
            val start = findFreeFrom(bm, at) ?: return 0
            if (runFree(bm, start, count)) {
                setBits(bm, start, count, set = true)
                usedFrames += count
                return MANAGED_BASE + start.toLong() * PAGE_SIZE
            }
            at = start + 1
        }
        return 0
    }

    /** 从 [from] 起第一个空闲位的下标；后面全满则返回 null。 */
    private fun findFreeFrom(bm: LongArray, from: Int): Int? {
        var p = from
        while (p < totalFrames) {
            val w = p / BITS_PER_WORD
            val b = p % BITS_PER_WORD
            val seg = bm[w] ushr b                  // b..63 归一到 0..
            if (seg != -1L) {
                /*
                 * 段里有空闲位。但 countTrailingZeroBits(0) == 64，对「本段整段全空」
                 * 来说会指到本字之外 —— 那样会白白跳过最多 64 帧。这里真的踩到过：
                 * 第一个可用帧恰好落在字边界，分配器每次都从下一个字开头返回，
                 * 表现为「每次分配都跳过一整块」。
                 */
                val t = seg.countTrailingZeroBits()
                return if (t < BITS_PER_WORD - b) p + t else p
            }
            p = (w + 1) * BITS_PER_WORD
        }
        return null
    }

    /** `[start, start+count)` 这些位是否全为 0（空闲）。 */
    private fun runFree(bm: LongArray, start: Int, count: Int): Boolean {
        var k = 0
        while (k < count) {
            val p = start + k
            val w = p / BITS_PER_WORD
            val b = p % BITS_PER_WORD
            // 本字从 b 起、到本字末尾（或到 count 结束）的连续位
            val take = minOf(BITS_PER_WORD - b, count - k)
            val mask = if (take >= BITS_PER_WORD) -1L else (1L shl take) - 1
            if ((bm[w] ushr b) and mask != 0L) return false
            k += take
        }
        return true
    }

    /** 把 `[start, start+count)` 这些位置位或清零。 */
    private fun setBits(bm: LongArray, start: Int, count: Int, set: Boolean) {
        var k = 0
        while (k < count) {
            val p = start + k
            val w = p / BITS_PER_WORD
            val b = p % BITS_PER_WORD
            val take = minOf(BITS_PER_WORD - b, count - k)
            val mask = if (take >= BITS_PER_WORD) -1L else (1L shl take) - 1
            bm[w] = if (set) bm[w] or (mask shl b) else bm[w] and (mask shl b).inv()
            k += take
        }
    }

    /** 分配一个页帧，等价于 `allocFrames(1)`。 */
    fun allocFrame(): Long = allocFrames(1)

    /**
     * 归还从 [physAddr] 开始的 [count] 个页帧。
     *
     * 落在管辖区间外的地址直接忽略（引导栈那种由内核自管的东西）。
     */
    fun freeFrames(physAddr: Long, count: Int) {
        if (count <= 0) return
        val bm = bitmap ?: return
        val start = (physAddr / PAGE_SIZE).toInt()
        val end = start + count
        for (p in start until end) {
            if (p < 0 || p >= totalFrames) continue
            val w = p / BITS_PER_WORD
            val mask = 1L shl (p % BITS_PER_WORD)
            if (bm[w] and mask != 0L) {
                bm[w] = bm[w] and mask.inv()
                usedFrames--
            }
        }
    }

    /** 某地址当前是否已用。 */
    fun isUsed(physAddr: Long): Boolean {
        val bm = bitmap ?: return true
        val p = (physAddr / PAGE_SIZE).toInt()
        if (p < 0 || p >= totalFrames) return true
        return bm[p / BITS_PER_WORD] and (1L shl (p % BITS_PER_WORD)) != 0L
    }

    /** 把分配器管辖的空间清零，用于刚拿到的大块内存。 */
    fun zero(addr: Long, length: Long) = zeroMemory(addr, length)

    private fun markRange(start: Long, end: Long, free: Boolean) {
        val bm = bitmap ?: return
        if (end <= start) return
        val lo = maxOf((start / PAGE_SIZE).toInt(), 0)
        val hi = minOf(((end + PAGE_SIZE - 1) / PAGE_SIZE).toInt(), totalFrames)
        for (p in lo until hi) {
            val w = p / BITS_PER_WORD
            val mask = 1L shl (p % BITS_PER_WORD)
            bm[w] = if (free) bm[w] and mask.inv() else bm[w] or mask
        }
        if (!free) usedFrames += (hi - lo)
    }

    /** 一次性的统计快照，供横幅打印。 */
    fun stats(): String {
        if (bitmap == null) return "物理页：未初始化"
        val totalMiB = usedBytes shr 20
        val capMiB = totalBytes shr 20
        return "物理页：$totalMiB / $capMiB MiB 已用（共 $totalFrames 帧）"
    }

    private const val FOUR_GIB = 0x1_0000_0000L
}

/** 4 GiB —— 引导桩线性映射的物理地址上界。 */
const val FOUR_GIB: Long = 0x1_0000_0000L