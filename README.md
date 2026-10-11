# KraOS

KraOS 是一个用 **Kotlin/Native（Konan）** 写的 **x86_64 裸机操作系统**。
内核没有 Linux 用户态：没有解释器、没有 `PT_INTERP`、没有动态链接段，
`make check` 会把这件事当成硬指标来断言。

它不是 Linux 发行版，也不是宿主环境中的桌面 Demo。当前处于**内核地基与早期桌面阶段**：
引导链、Kotlin framebuffer 桌面首屏与键盘应用切换已跑通；窗口管理、可编辑终端/文件、
鼠标、驱动和持久文件系统仍在实现中。

---

## 快速开始

```bash
cd KraOS

make          # 构建（约 2–3 分钟）
make run      # 在 QEMU 里启动，串口输出直接打在终端
make check    # 验收断言：无 PT_INTERP / 入口链 / 无动态段 / 无 ifunc 桩
```

`make run` 等价于：

```bash
qemu-system-x86_64 -machine pc -cpu max -m 256M \
  -cdrom build/kraos.iso -boot d -display none -serial stdio -nic none -no-reboot
```

启动后串口上应当看到：

```text
[boot] Multiboot2 入口，正在准备 x86_64 长模式
[boot] long mode 可用
[boot] 已开启 PAE/LME/PG
[boot] 已进入 64 位长模式
[boot] 移交 Konan_main
```

出现 `[boot] 移交 Konan_main` 就说明 **Kotlin 代码已经在裸机上真实执行**
（`Konan_main` 是 K/N 运行时的 Kotlin 入口）。

### 排障

| 现象 | 原因 |
| --- | --- |
| 启动没有任何输出 | ISO 没被 GRUB 认成 multiboot2；先跑 `make check` |
| 输出停在中途 | 串口波特率固定 115200 8N1，无流量控制 |
| 看到 `[kraos] 异常 vector=…` | 内核真的崩了；这一行后面紧跟出错指令、出错地址和全部寄存器 |
| 构建「成功」但行为没变 | gas 的报错是中文的（`错误`），`make` 不会因 gas 失败而停；务必 grep `'错误\|error:'` |

深入排障、失败现场逐条解读、以及本阶段踩过的所有坑，见
[`HANDOFF.md`](HANDOFF.md) §11。

---

## 项目结构

```text
KraOS/
├── Makefile                 构建 / run / check / 镜像
├── boot/                    引导与最小运行时基座（K/N 内联汇编不可用，全在这里）
│   ├── entry.S              32 位 multiboot2 入口 → 长模式；GDT、IDT、SSE、
│   │                        系统调用入口、FS 基址、串口、64 MiB 启动堆
│   ├── libc_shim.c          brk/sbrk/mmap 堆、系统调用分发、IRELATIVE 应用、
│   │                        IDT 构建、故障诊断打印
│   ├── ifunc_shim.c         35 个字符串/内存函数的自研实现，消解 glibc ifunc
│   ├── linker.ld            段布局、PT_TLS、.rela.iplt
│   └── grub.cfg             GRUB 菜单与 multiboot2 装载
├── kernel/src/              Kotlin 侧内核代码
│   ├── Main.kt              入口与自检横幅
│   └── boot/                multiboot2 解析、启动堆、自检
├── docs/
│   ├── PLAN-kraos-kotlin-native.md   分阶段计划与验收标准
│   └── ARCHITECTURE.md              实现结构与边界
├── tests/
└── toolchain/
    └── konan-patch/         对 Kotlin/Native 的源码级改动（新增 kraos_kernel 目标）
```

完整的阶段交接、决策记录与验证证据在 [`HANDOFF.md`](HANDOFF.md)。

---

## 工具链

KraOS 通过**修改 Kotlin/Native 源码**新增两个自定义目标，而不是对二进制 jar 做手术：

| 目标 | 定位 | 状态 |
| --- | --- | --- |
| `kraos_kernel` | 裸机内核，freestanding，无 libc 启动约定 | 已实现并在 QEMU 上运行 |
| `kraos` | 用户态 KraELF 容器 | **仅占位**，格式与加载器尚未设计 |

改动固化在 [`toolchain/konan-patch/kraos-targets.patch`](toolchain/konan-patch/kraos-targets.patch)
（相对上游 v2.1.21 纯净树，616 行、7 个文件，已验证可完整复现当前源码树）。
构建与部署步骤见 [`toolchain/konan-patch/README.md`](toolchain/konan-patch/README.md)。

---

## 当前边界

**已经做到**

- multiboot2 引导 → 32 位 → 长模式 → `Konan_main`，Kotlin 代码在裸机上真实执行
- 自建 GDT / IDT，异常在串口上打印向量号、错误码、RIP、CR2、全部寄存器与现场内存
- `syscall` 门与 C 侧系统调用分发（write/brk/mmap/arch_prctl/clock/getrandom…）
- 64 MiB 启动堆（SSE 状态、IDT、栈、TLS 块都在其中）
- 产物为静态 ELF：无 `PT_INTERP`、无 `DYNAMIC`、无 ifunc 桩

**还没有**

- 物理页分配器、内核堆（现在只有 64 MiB 固定启动堆）
- 调度器、进程、用户态与 `pthread_*`
- 真正的 K/N 运行时位码（目前借用 `linux_x64` 的 `.bc`）
- 驱动、文件系统、网络、图形

**P1 已完成**：内核能引导进长模式、跑起 K/N 运行时、由 Kotlin 代码在裸机上
逐条解析 multiboot2 内存图，并打印验收横幅

```text
KraOS boot ok  mem=255 MiB  cpu=1
```

CPU 核数随 `-smp` 正确变化（已验证 1/2/4/8）。**在 QEMU 默认 CPU 模型下同样可启动**
（曾因无条件写 `CR4.OSXSAVE` 而在 `-cpu max` 以外静默三重故障，见 `HANDOFF.md` §11.3）。
曾长期阻塞的 K/N 初始化问题，根因与定位过程见 `HANDOFF.md` §11.4——结论是引导桩的
`syscall_entry` 破坏了调用者的 `%rbx`（被调用者保存寄存器），与 TLS 布局本身无关。
P2 的已知欠账见 `HANDOFF.md` §11.6。

没有安装器，也不向真实磁盘写入系统。现阶段只在 QEMU 里运行。

---

## 许可

本项目代码使用 **BSD 3-Clause** 许可（Copyright (c) 2026 CryNet Studio by RJ）。
详见 [`LICENSE`](LICENSE)。
