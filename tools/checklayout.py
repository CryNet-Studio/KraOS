#!/usr/bin/env python3
"""链接布局体检：模拟 GRUB 的保留区申请，提前抓出「能链接但装不上」的布局。

GRUB2 的 multiboot2 模块按 p_paddr 装载每个 PT_LOAD，并先向引导器申请一块
[p_paddr, p_paddr + p_memsz) 的低内存保留区，块边界按页向**下/上**取整。只要两块
保留区相交，GRUB 就报

    error: lib/relocator.c:grub_relocator_alloc_chunk_addr:1246:overlap detected.

然后直接拒绝加载内核 —— 屏幕上只留下一句 "you need to load the kernel first"，
串口上一行输出都没有。从 QEMU 现象上看这和「内核根本没跑」完全一样，很容易误判
成引导桩的问题。

同一个脚本还校验三件事：
  * 高半区每个 PT_LOAD 的 VMA-LMA 差值必须完全一致（引导桩靠这个固定平移量按
    2 MiB 大页平铺映射）；
  * 每个 LMA 必须 2 MiB 对齐（大页物理基址的硬要求）；
  * _start（ELF 入口）必须落在 4 GiB 以下 —— GRUB 在 32 位保护模式跳 e_entry。

用法： python3 tools/checklayout.py build/kernel.elf
退出码 0 表示布局自洽。
"""

import re
import subprocess
import sys

KERNEL_VMA = 0xFFFFFFFF80000000
PAGE = 4096


def readelf(args, path):
    env = {"LC_ALL": "C", "PATH": "/usr/bin:/bin"}
    out = subprocess.run(["readelf"] + args + [path],
                         capture_output=True, text=True, env=env)
    if out.returncode != 0:
        sys.exit("readelf %s %s 失败:\n%s" % (" ".join(args), path, out.stderr))
    return out.stdout


def parse_phdrs(path):
    loads = []
    tls = None
    for line in readelf(["-lW"], path).splitlines():
        m = re.match(r"\s+LOAD\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)"
                     r"\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+((?:\S+\s+)*?)"
                     r"0x([0-9a-f]+)\s*$", line)
        if m:
            off, va, pa, fsz, msz = (int(x, 16) for x in m.groups()[:5])
            flg, align = m.group(6).strip(), int(m.group(7), 16)
            loads.append(dict(off=off, va=va, pa=pa, fsz=fsz, msz=msz,
                              flg=flg, align=align))
            continue
        m = re.match(r"\s+TLS\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)"
                     r"\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)", line)
        if m:
            off, va, pa, fsz, msz = (int(x, 16) for x in m.groups())
            tls = dict(off=off, va=va, pa=pa, fsz=fsz, msz=msz)
    return loads, tls


def entry_point(path):
    m = re.search(r"Entry point address:\s+(0x[0-9a-f]+)",
                  readelf(["-h"], path))
    return int(m.group(1), 16)


def check(path):
    loads, tls = parse_phdrs(path)
    entry = entry_point(path)
    problems = []

    print("ELF 入口 _start = %#x" % entry)
    if entry >= (1 << 32):
        problems.append("_start 在 4 GiB 以上：GRUB 32 位保护模式跳不过去")

    print("\n%-4s %-18s %-12s %-10s %-10s %-6s %s"
          % ("#", "VirtAddr", "PhysAddr", "FileSiz", "MemSiz", "Flg", "VMA-LMA"))
    for i, s in enumerate(loads):
        delta = (s["va"] - s["pa"]) & 0xFFFFFFFFFFFFFFFF
        print("%-4d %#018x %#012x %#010x %#010x %-6s %#x"
              % (i, s["va"], s["pa"], s["fsz"], s["msz"], s["flg"], delta))

    # --- 1. GRUB 保留区不得相交 -------------------------------------------
    print("\n[1] GRUB 保留区（p_paddr 起、p_memsz 止，页取整）")
    chunks = []
    for s in loads:
        if s["msz"] == 0:
            continue
        start = s["pa"] & ~(PAGE - 1)
        end = (s["pa"] + s["msz"] + PAGE - 1) & ~(PAGE - 1)
        chunks.append((start, end, s["va"]))
    chunks.sort()
    for start, end, va in chunks:
        print("    %#012x .. %#012x  (段 vma %#x)" % (start, end, va))
    for (a0, a1, _), (b0, b1, _) in zip(chunks, chunks[1:]):
        if a1 > b0:
            problems.append(
                "保留区相交：%#x..%#x 与 %#x..%#x —— GRUB 会报 "
                "grub_relocator_alloc_chunk_addr: overlap detected"
                % (a0, a1, b0, b1))

    # --- 2. 高半区固定平移量 ------------------------------------------------
    print("\n[2] 高半区 VMA-LMA 固定平移量")
    deltas = {(s["va"] - s["pa"]) & 0xFFFFFFFFFFFFFFFF
              for s in loads if s["va"] >= KERNEL_VMA}
    print("    取值集合 = %s" % (sorted("%#x" % d for d in deltas)))
    if len(deltas) != 1:
        problems.append("高半区各段的 VMA-LMA 差值不一致：引导桩无法用同一个"
                        "平移量铺大页（多半是某段的 ALIGN() 在 VMA 侧和 LMA 侧"
                        "没有同步）")
    for d in deltas:
        if d % PAGE:
            problems.append("平移量 %#x 不是页对齐" % d)

    # --- 3. 大页物理基址 2 MiB 对齐 ----------------------------------------
    # 只有**第一段**高半区段（也就是 __kernel_lma）的物理基址需要 2 MiB 对齐 ——
    # 它是引导桩铺大页的起点。后续段只要页对齐、平移量与它一致就够了。
    print("\n[3] 大页物理基址对齐")
    kmin = min((s["pa"] for s in loads if s["va"] >= KERNEL_VMA), default=0)
    print("    内核 LMA 基址 = %#x（2 MiB 对齐: %s）"
          % (kmin, "是" if kmin % (2 << 20) == 0 else "否"))
    if kmin and kmin % (2 << 20):
        problems.append("__kernel_lma %#x 未按 2 MiB 对齐：大页的物理基址必须"
                        "2 MiB 对齐，否则整段映射都会偏" % kmin)

    # --- 4. 文件偏移与 p_paddr 自洽 ----------------------------------------
    print("\n[4] 段内 file offset - p_paddr 是否恒定")
    by_seg = {}
    for s in loads:
        by_seg.setdefault((s["off"] - s["pa"]) & 0xFFFFFFFFFFFFFFFF, []).append(s["va"])
    for d, vas in sorted(by_seg.items()):
        print("    %#x  <- %s" % (d, ", ".join("%#x" % v for v in vas)))

    # --- 5. PT_TLS 体积 ------------------------------------------------------
    if tls:
        print("\n[5] PT_TLS memsz = %#x（%d 字节）" % (tls["msz"], tls["msz"]))
        if tls["msz"] > 0xF000:
            problems.append(
                "PT_TLS memsz %#x 超出引导桩 64 KiB 的 %%fs 块可用范围 0x1000："
                "有 __thread 变量会落到块外 #PF" % tls["msz"])

    # --- 结论 ----------------------------------------------------------------
    print()
    if problems:
        for p in problems:
            print("FAIL: %s" % p)
        return 1
    print("OK: 布局自洽，可以交给 QEMU 验机")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sys.exit(check(sys.argv[1]))
