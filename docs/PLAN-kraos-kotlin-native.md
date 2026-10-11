# KraOS 计划：全面改名 + Kotlin/Native 自造工具链重写

状态：**改名部分待实施；工具链部分已开工**（详见文末「双目标拆分」后的进展说明）
记录时间：2026-10-02
前置计划：`docs/PLAN-storage-driver.md`（存储与驱动层）

## 已确认决策

| 决策点 | 结论 |
| --- | --- |
| 执行顺序 | **方案 B：Kotlin 优先**——改名后直接用 Kotlin 实现（含存储栈），不在 C 侧先做参考实现 |
| 改名深度 | 改代码标识符前缀 `ark_` → `kra_`、宏 `ARK_` → `KRA_`、`include/ark.h` → `include/kra.h` |
| 版本号 | `0.1.0` → **`0.2.0`**（ISO、目录、清单同步） |
| K1 工具链 PoC | **不设回退、硬磕到底**：坚持自造 custom target（含自建 stdlib klib），混合方案不采用 |
| KN 内置 GC 怎么处理 | **分两步**：先用 `-Xbinary=gc=noop` 关掉内置 GC 把内核跑起来（P1–P3）；到 P4 再自研 mark-sweep 接管（详见下文 GC 决策） |
| 引导桩用汇编 | **必须**。K/N 2.1.255 没有可用的内联汇编，所有特权指令只能在引导桩里用 C ABI 暴露 |

本计划包含两件事，彼此独立但要考虑先后关系：

1. **全面改名 KraOS**，包括代码标识符；
2. **用 Kotlin/Native 重写内核**，并自造一套自定义工具链（依据：Kotlin/Native 产物本质是 LLVM 码）。

> **工具链部分当前进展**：编译侧改造已落地为**双目标**——`kraos_kernel`（freestanding 内核）与
> `kraos`（用户态 KraELF，占位）。`kraos_kernel` 的 freestanding 链接**已实现并验证**
> （EXEC、无 PT_INTERP）。
>
> **2026-10-04 更新**：内核已经**在 QEMU 里跑起来** —— multiboot2 引导 → 长模式 → SSE →
> 系统调用 → FS 基址 → `Konan_main`，K/N 运行时开始在裸机上执行，`make check` 四条断言全绿。
> 当前卡在 K/N TLS 布局（见「第零部分」）。自研运行时位码、KraELF 格式、K0 改名、
> qemu/xorriso **尚未完成**。

---

## 第零部分：内核交付分期（P1–P4）与 GC 决策

> 这是 2026-10-04 起的实际执行分期。前面的 K0–K5 是更早的粗粒度规划；
> 真正在推进的是下面这张表，两者的关系是：K1 ≈ P1，K2/K3 ≈ P4，K4 ≈ P4 之后。

| 阶段 | 内容 | 通过标准 | 状态 |
| --- | --- | --- | --- |
| **P1** | 引导链 + 最小运行时基座：multiboot2 → 长模式 → SSE → `syscall` → FS 基址 → `Konan_main` | ① QEMU 串口出现 `KraOS boot ok`（含内存大小与 CPU 核数）<br>② `readelf -l` 无 `PT_INTERP`，控制权交给 `Konan_main`<br>③ Kotlin 代码在裸机上真实执行（串口可见其输出），非仅链接通过 | ✅ **3/3 已达成** |
| **P2** | 物理页分配器（基于 multiboot2 内存图）；摘掉 glibc；切回高半区 | `kraosVirtualAddress` 切回 `0xffffffff80000000` 且能启动 | ✅ **完成**：页分配器 ✅（自检全过）；glibc 归档已摘 ✅（30 个符号由 `boot/libc_min.c` 自研顶上，产物 −21%）；高半区 ✅（内核主体跑在 `0xffffffff80000000`，引导矩阵 6/6 + `make check` 5/5，见 `HANDOFF.md` §12.3） |
| **P3** | 调度器 + 26 个 `pthread_*` shim；GDT/IDT/PIC/APIC 完整化 | 多任务可抢占切换；中断可路由 | 🟡 IDT/GDT 已有雏形（仅异常向量） |
| **P4** | 自研 K/N 运行时位码 + stdlib 平台 actual；**自研 mark-sweep GC** | 标准库可用；GC 可回收 | ⬜ |

### GC 决策（K/N 内置 GC 怎么处理）

K/N 的 GC 在 linux_x64 上是**并行标记-清扫 + 并行复制**，依赖线程、原子操作、
`mmap` 支撑的堆外内存、以及停世（stop-the-world）机制。这些在裸机 P1–P3 阶段
一个都不具备。

因此分两步：

1. **P1–P3：关掉内置 GC**，目标属性加 `-Xbinary=gc=noop`。
   内存只增不减，由启动堆承担。好处是把「GC 能不能跑」这个变量从
   「内核能不能引导」里彻底剥离出去 —— 否则每次崩溃都要先分辨
   「是引导错了」还是「是 GC 错了」。
2. **P4：自研 mark-sweep**，接回 `kotlin.native.internal.GC` 的接口。
   单线程、非分代、全量标记-清扫，配一个 bump 分配器与显式 `GC.collect()`。
   选 mark-sweep 而不是复制式，是因为它不需要额外的 from/to 空间，
   在没有 mmap 支撑的堆上更容易做对。

> 风险记录：`gc=noop` 意味着**任何逃逸分配都不会被回收**。P3 之前无所谓（没有循环分配），
> 但 P3 一旦引入调度与周期性分配，就必须先有 P4 的 GC，或者至少有一个显式的
> 「重建堆」策略，否则长时间运行必然耗尽内存。

### P1 当前的唯一阻塞点

K/N 线程本地存储（TLS）布局与我们在引导桩里手工构造的 FS 基址不一致：
`ThreadLocalStorage::AddRecord` 拿到的 `this` 是栈地址而不是 `&ThreadData::tls_`。
现象、反汇编证据、三条候选根因与建议动作全部记录在 [`../HANDOFF.md`](../HANDOFF.md) §11.4。

**它是 P1 验收项①（横幅输出）的前置条件**，因此是当前最高优先级。
注意横幅本身已经写好了，卡的纯粹是运行时初始化。

---

## 第一部分：全面改名 KraOS

### 命名映射（待确认）

| 类别 | 旧 | 新 |
| --- | --- | --- |
| 品牌显示名 | `ArkOS` | `KraOS` |
| 小写标识 | `arkos` | `kraos` |
| 代码前缀（类型/函数/变量） | `ark_` | `kra_` |
| 宏前缀 | `ARK_` | `KRA_` |
| 头文件 | `include/ark.h` | `include/kra.h` |
| 头文件名宏 | `ARK_H` | `KRA_H` |
| 串口/UI 标签 | `[arkos]` | `[kraos]` |
| Shell 提示符 | `ark>` | `kra>` |
| 产物 | `build/arkos-0.1.0.iso` | `build/kraos-0.2.0.iso` |
| 顶层目录 | `ArkOS-0.1.0/` | `KraOS-0.2.0/` |

### 影响面（实测）

全仓库约 **86 处命中、21 个文件**（含 `build/` 生成物与 `docs/verification/` 历史日志）。需要改的实体文件集中在：

- 代码：`include/ark.h`、`kernel/*.c`
- 构建/打包：`Makefile`、`boot/grub.cfg`（menuentry 文案）、`boot/README.md`
- 脚本：`scripts/run-qemu.sh`、`scripts/run-qemu.ps1`
- 测试：`tests/boot_test.py`（断言 `ArkOS 0.1.0 x86_64`、`[arkos] desktop ready`、ISO 路径）
- 文档：`README.md`、`docs/ARCHITECTURE.md`、`docs/VALIDATION.md`
- 清单：`SHA256SUMS.txt`（改名后需重算；`LICENSE` 已重写为 BSD 3-Clause，不再含旧命名）

### 执行要点

- **本仓库当前不是 git 仓库，且明确不在全部完成前 `git init`**（用户已确认）。因此改名用普通文件重命名/重写，**不使用 `git mv`**，也没有提交可回溯；动手前先整目录备份。
- 替换顺序要避免自伤：先长 token 后短 token（`ArkOS` → `arkos` → `ARK_` → `ark_`），最后处理裸 `ark`/`Ark`，并逐条人工复核，**不要**全局无脑 `sed`。
- `third_party/` **已整体删除**（vendored GNU GRUB：tarball / 打包 / `COPYRIGHT` / `GPL-3.txt`），改名时不再涉及；备份在仓库外。后续若 vendored Limine，放 `third_party/limine/`（BSD-2-Clause）。
- `build/` 与 `docs/verification/` **全部清理掉，不做就地替换**：`build/` 用 `make clean` 删除；`docs/verification/`（`bios-serial.log`、`uefi-serial.log` 与 4 张截图）整体清除，改名后重做。注意本仓库无 git，清理前先在仓库外留一份备份。
- 改完后 `make && python3 tests/boot_test.py bios` 必须全绿，作为 Kotlin 重写前的基线。
- **`git init` 是收尾动作**：放在 K5（全 Kotlin 启动 + 回归通过 + C 源码移除）之后，作为整件事彻底完成后的最后一步，届时再做基线提交。

### 已确认

- 版本号提升到 `0.2.0`。
- 代码前缀改为 `kra_`/`KRA_`（含 `include/ark.h` → `include/kra.h`）。

---

## 第二部分：Kotlin/Native 重写 + 自造工具链

### 思路与可行性边界

**成立的部分**：Kotlin/Native 的前端把 Kotlin 降到 LLVM IR，再交给 LLVM 做 codegen。既然产物是 LLVM 能消费的中间表示，原则上可以指定自定义 target triple、替换链接器与 runtime，把它接到我们自己的 `boot/entry.S` + `linker.ld` + Multiboot2 上。

**必须承认的难点**：

1. Kotlin/Native **没有「无 runtime」模式**。`stdlib` 依赖内存管理器（allocator/GC）、原子操作、异常、少量 libc 语义。
2. 每个 target 需要**预编译的 platform klib**（`stdlib` 等）。JetBrains 官方不含 freestanding 目标，**必须自建**——这正是「自造工具链」的核心工作量。
3. Kotlin/Native 默认带 **worker 线程模型**；内核里要么用单线程 memory model 并关掉并发特性，要么提供线程 stub。
4. 工具链**版本强耦合**：kotlin-native 自带特定版本 LLVM，不能与系统 clang 混用。

### 工具链组成（自造部分）

```
toolchain/
  konan/konan.properties        新增 kraos_kernel / kraos 两个目标条目，由 deploy-to-dist.sh 并入
  targets/kraos_kernel/         freestanding 内核目标定义 + cinterop 描述（绑定内核 C ABI）
  targets/kraos/                用户态 KraELF 目标定义（占位，容器格式未实现）
  build-runtime.sh              用 kraos_kernel 编译 K/N runtime/stdlib，产出 klib
runtime/
  allocator.c                   Kotlin 分配器 → 接到 P0 的 bump 分配器
  panic.c                       assert/异常/abort → 内核 panic()
  io.c                          println/输出 → 现有 serial_write / shell_print
```

**双目标拆分**：KraOS 注册两个目标——`kraos_kernel` 是 freestanding 内核（裸机，无 libc 启动），
`kraos` 是用户态 KraELF 容器目标。二者都保留 `Family.LINUX` 以复用工具链/sysroot 解析，
差异全部收敛在 konan.properties 与 `KraOSLinker` 里。`kraos_kernel` 的 freestanding 链接已实现并验证；
`kraos` 侧 KraELF 格式与加载器**尚未实现**。

目标条目的关键字段（最终目标形态；当前 bootstrap 阶段借用 linux_x64 工具链，实际值见 6.2）：

- `llvmTarget` / triple：最终 `x86_64-unknown-none`（当前暂用 `x86_64-unknown-linux-gnu`）
- `targetToolchain`、`linker`：指向 Kotlin/Native 自带 LLVM 工具，不用系统 clang
- `targetSysRoot`、`abiVersion`、`dependencies`：freestanding，无 libc

### 构建管线

```
Kotlin 源
  → kotlinc-native -target kraos_kernel  (自定义 konan.properties + KraOSLinker)
  → LLVM IR / 目标文件
  → llvm-link + runtime 桩（自写）
  → ld.lld --entry=Konan_main -T <kraos linker script> --no-dynamic-linker -static ...
  → build/kraos-0.1.0.iso
```

迁移期用 C ABI 桥共存：Kotlin 侧用 `@CName` / `-Xexport` 暴露 `kernel_main`，C 侧保留 `boot/entry.S`、`kernel/interrupts.S` 与最底层平台代码。中断汇编与引导代码**不重写**（Kotlin 不产生裸汇编入口）。

### 分阶段

| 阶段 | 内容 | 通过标准 |
| --- | --- | --- |
| **K0** | 改名 KraOS（第一部分），保证基线全绿 | `make` + BIOS/UEFI 测试通过 |
| **K0b** | 引导协议改造（第五部分）：ISO 换 `xorriso` + Limine 去 GRUB；双镜像（`-e`）支持 Limine + multiboot2 | BIOS/UEFI 在 Limine 下全绿；`kernel-mb2.elf` 能在 GRUB 下启动；产物零 GPL |
| **K1** | 工具链 PoC：装 kotlin-native、写自定义 target、编一个只打印串口的 freestanding 程序 | QEMU 里能看到 Kotlin 输出的串口行 |
| **K2** | 自建 stdlib/runtime klib | Kotlin 标准库可用（至少字符串/数组/整数） |
| **K2b** | ASM 最小化：`interrupts.S` 宏化（1574 → ≤100 行），新增 `ctx.S` | 改前/改后 `objdump` 逐条等价；ASM 总量降到 **≈305–355 行** |
| **K3** | runtime 端口：allocator / panic / 输出 / **线程与同步原语（Worker 所需）** | 与 K2 组成可用的裸机 Kotlin 运行时，Worker 能 `start()`/`execute()` |
| **K4** | 逐模块实现（含存储栈）：`lib` → `mm/pci/driver/block/ata_pio` → `arkfs` → `shell` → `desktop` | 每步保留可启动的 C 基线，测试不回归 |
| **K5** | 全 Kotlin 启动 + 回归 + 删除 C（保留汇编） | BIOS/UEFI 全绿，C 源码移除 |

**K1 定为「硬磕」的里程碑，不是 Go/No-Go 门**：已确认**不设回退**，坚持自造 custom target 并自建 `stdlib` klib。混合方案（C 内核 + Kotlin 上层）**不采用**。

代价与应对：K1 若长时间受阻，应缩小验证面（先只跑整数与输出，逐个补齐 stdlib 依赖），而不是切换架构；遇到具体缺失的 runtime 原语，在 `runtime/` 下逐个补桩。

### 风险

1. **整体风险极高**：Kotlin/Native 并非为裸机设计，stdlib 的隐式依赖可能反复踩坑。
2. **版本锁定**：工具链版本一旦选定就不能随意升级，自建 klib 必须与编译器版本严格匹配。
3. **体积与性能**：stdlib 会显著增大 `kernel.elf`；`mem`/About 的内存语义需要重新定义。
4. **无内存保护**：Kotlin 运行时一旦越界直接崩内核，调试手段仍只有串口与异常日志。
5. **与存储计划的耦合**：Kotlin 重写会重做一遍 C 侧刚实现的存储栈，存在返工。

---

## 第三部分：为 Kotlin/Native Worker 实现线程

Kotlin/Native 的 `Worker`（`kotlin.native.concurrent.Worker`）**就是 OS 线程**：`start()` 起线程、`execute()` 投递任务、`requestTermination()` 结束。新内存模型下对象可跨 worker 共享，但**线程本体、TLS、互斥/条件变量、原子操作、等待/唤醒**这些平台原语必须由我们提供。因此「支持 Worker」=「在内核里造一个最小线程基座」。

### 需要实现的平台原语

| 原语 | 我们怎么做 |
| --- | --- |
| 线程创建/等待/退出 | 最小调度器（见下） |
| 线程本地存储 TLS | 每线程设置 `IA32_FS_BASE`（`wrmsr`），KN runtime 靠它存 ThreadData |
| 互斥、条件变量 | 逻辑用 Kotlin 写；底层阻塞/唤醒接调度器等待队列 |
| 原子操作 | 用 Kotlin/C++ atomic → clang 生成 `lock` 前缀，**无需 ASM** |
| 单调时间 | 复用已有 PIT tick |
| 内存映射 | 接到 P0 的 bump 分配器 |
| 输出 / abort | 桥到 `shell_print` / `serial_write` / 内核 `panic()` |

### 最小线程基座（去掉「没有调度器」这个前提）

1. **TCB 与固定线程池**：bump 分配器不回收，所以 TCB 用静态数组、线程栈用固定池（每线程 16–32 KiB），Worker 反复创建的上限即池大小。
2. **就绪队列 + 轮转调度**：简单 round-robin，PIT 100 Hz（已有）做时间片，实现**抢占式**多线程。
3. **阻塞/唤醒**：等待队列支撑 mutex/condvar/sleep，被阻塞线程让出 CPU。
4. **上下文切换**：唯一新增 ASM，见第四部分。
5. **和现有桌面循环的调和**：当前 `desktop.c` 是单前台轮询循环并 `hlt` 空闲；改造为「桌面是调度器里的一个线程」，`platform_idle` 语义并入空闲线程的 `hlt`。
6. **尽量不引入额外运行时线程**：用 `-Xallocator=custom` + noop GC，避免 KN runtime 再起 GC/并发线程，把「线程」需求收敛到 Worker 本身加极少量 runtime 内部线程。

### 风险

1. **抢占与 KN runtime 的线程本地假设冲突**：runtime 假定某些状态线程独占，抢占点选择不当会出隐蔽 bug。
2. **栈池会耗尽**：bump 不回收 + 固定池，长时间反复创建 Worker 最终失败；需明确上限与错误返回。
3. **竞态/死锁难调**：仍只有串口与异常日志，没有调试器。

---

## 第四部分：自造 KN 工具链/运行时 + 内核 ASM 最小化

### 4.1 runtime 平台后端自实现

Kotlin/Native 的 runtime 是按平台分后端实现的（自带 `posix`、`windows`、`zephyr`、`wasm` 等）。我们**新增一个 `kraos` 后端**，把第三部分那张原语表逐项填上。

**可执行的推进方法**（不靠猜符号名）：

1. 定义自定义 target（第二部分 K2）；
2. 用该 target 编译 runtime/stdlib；
3. **链接时收集未定义符号**，得到一份「runtime 到底要什么」的真实清单；
4. 在 `runtime/platform/` 逐个补桩，迭代到链接通过。

runtime 目录组成：

```
runtime/
  platform/    线程、互斥、条件变量、TLS、时间、映射、输出、abort
  alloc/       分配器 → bump（另加线程栈/对象的小型 free-list）
  gc/          noop（避免 GC 线程与 safepoint）
  exception/   关闭，panic 映射到内核
  io/          println → shell_print / serial_write
```

### 4.2 ASM 最小化（基于实测数据）

当前汇编规模（实测）：

| 文件 | 行数 | 说明 |
| --- | --- | --- |
| `boot/entry.S` | 197 | 32 位入口、长模式、静态页表 |
| `kernel/interrupts.S` | **1574** | 256 个手写 stub + 256 项函数表 |

**`interrupts.S` 是最大的一块浪费**：256 个 stub 里只有 8/10/11/12/13/14/17/21/29/30 带错误码，其余一律 `pushq $0` + `pushq $n` + `jmp isr_common`；末尾 256 项 `.quad isr_n` 表同样是机械重复。用 GAS 宏（`.irp` / `.rept` + `.if`）可生成全部 256 个 stub 与整张表，**行为完全不变**。

目标 ASM 预算：

| 文件 | 现状 | 目标 | 手段 |
| --- | --- | --- | --- |
| `boot/entry_mb2.S`（原 `entry.S`） | 197 | **~90** | 只留 multiboot2 必需的 32 位→64 位 trampoline，删早期串口等可选路径 |
| `boot/entry_limine.S`（新增） | 0 | **~25** | 64 位 stub：设栈 → 调 C 初始化 → 跳 `kernel_main`；页表/IDT 初始化移到 C |
| `kernel/interrupts.S` | 1574 | **≤100** | 全部 stub 与表改为宏生成 |
| `kernel/ctx.S`（新增） | 0 | **60–100** | 上下文切换 + 线程 trampoline（第三部分的唯一新增 ASM） |
| `kernel/asmio.S`（可选） | 0 | ~30 | `inb/outb/wrmsr/rdmsr` 等端口与 MSR 单点封装 |
| **合计** | **1771** | **≈305–355** | 净减少约 **80%** |

硬约束：

- **32 位代码只允许存在于 `entry_mb2.S` 一个文件**，且只做「保护模式 → 长模式」切换，不得承载任何内核逻辑。
- 新增 64 位 ASM 只允许出现在 `entry_limine.S` 与 `ctx.S`（+ 可选的 `asmio.S`）；调度、队列、TLS 记账、锁、Worker 逻辑全部 Kotlin。
- 改 `interrupts.S` 前后各存一份 `objdump -d`，**逐条比对反汇编**确认等价，再谈减少行数。
- 端口 I/O 与 MSR 访问集中到单一文件，禁止散落内联汇编。

---

## 第五部分：多协议引导（Limine 主 + Multiboot2 兼容）

### 决策（已确认）

- 内核主体**完全原生 64 位**，不设 32 位目标；**唯一的 32 位代码**是 multiboot2 规范强制要求的那段最短 trampoline。
- 支持两种引导协议：**Limine（主，随 ISO 分发）** 与 **Multiboot2（兼容，供 GRUB 等第三方引导器）**。
- **不做 multiboot1**；**不做 FreeBSD loader**（前者规范即为 32 位入口模型，后者不是本项目目标）。

### 核心矛盾与解决

| 协议 | 入口模式 | 交接约定 |
| --- | --- | --- |
| Multiboot2 | **32 位保护模式** | `EAX=0x36d76289`，`EBX`=信息结构地址 |
| Limine | **64 位长模式** | `rip`=ELF 入口，无参数；数据靠 `.limine_requests` 的 response |

Limine v12.9.1 `PROTOCOL.md`「Machine State at Entry / x86-64」实测要求：`CS=0x28`、`DS/ES/SS/FS/GS=0x30`（基址归零）、**GDT 由引导器加载**、**IDT 状态未定义（必须自建）**、`IF`/`DF` 清零、`PE/PG/PAE/LME/LMA` 已置位、PIC 全屏蔽。

两者对 `e_entry` 的**模式要求互斥**，单个 ELF 无法同时满足。解决方式：**同一份目标文件、两次链接、只换 `-e`**。

- `build/kernel-mb2.elf`：`ld -e _start_mb2`（32 位 trampoline）
- `build/kernel-limine.elf`：`ld -e _start_limine`（64 位 stub）
- 两镜像共享**全部**内核目标文件，内核主体字节一致；`.multiboot2` 头与 `.limine_requests` 段可同时存在且互不干扰（GRUB 忽略后者，Limine 忽略前者）。
- **发布的 ISO 只装 `kernel-limine.elf`** → 产物零 GPL。

### 引导协议归一化

- 新增归一化层：`_start_mb2` / `_start_limine` 各自把引导数据填进同一个 `BootInfo`，此后**内核不再感知引导器**。
- 迁移方式：先把现有 `parse_multiboot()` 拆成「协议解析 → 填 `BootInfo`」两段，再加 `parse_limine()`；[platform.c](file:///workspace/ArkOS/ArkOS-0.1.0/kernel/platform.c#L80-L155) 的 `BootInfo` 字段与那批校验（32 位 RGB、几何范围、4 GiB 上限、通道不重叠）**复用不变**。
- Limine 侧请求：`base_revision` / `framebuffer` / `memmap` / `kernel_address`（物理=虚拟=1 MiB）。**刻意不依赖 HHDM**：stub 自建 identity 页表，保持现有「物理指针可直接用」的内核模型，避免全内核改写指针算法。
- 请求区需 8 字节对齐的 start/end 标记；base revision ≥2 起引导器**必须**遵守该标记（v12 现状）。

### 镜像构建（官方配方）

出自 v12.9.1 `USAGE.md`「BIOS/UEFI hybrid ISO creation」：

- 根目录放 `limine.conf`、`limine-bios.sys`、`limine-bios-cd.bin`、`limine-uefi-cd.bin`，并建 `EFI/BOOT/BOOTX64.EFI`
- `xorriso -as mkisofs -R -r -J -b <limine-bios-cd.bin> -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus -apm-block-size 2048 --efi-boot <limine-uefi-cd.bin> -efi-boot-part --efi-boot-image --protective-msdos-label <root> -o image.iso`
- 收尾必须 `limine bios-install image.iso`（BIOS 混合启动）

Limine v12.9.1 二进制为 **BSD-2-Clause**，可正规 vendored 到 `third_party/limine/`（含其 LICENSE）。

### 授权结果

- 发布 ISO 由 **Limine（BSD-2-Clause）** 相伴，**GRUB/GPLv3 彻底退出产物**。
- 内核保留 multiboot2 头属于**兼容性**：multiboot 规范本身非 GPL，不引入授权负担；GRUB 此后只是「用户自选的兼容引导器」，**不再分发**。

---

## 现状勘察（本机环境）

| 组件 | 状态 |
| --- | --- |
| `gcc` 11.4 / `ld` 2.38 | 已有（C 基线可构建） |
| `java` | OpenJDK **11**（较新 K/N 可能需要 JDK 17，需补装） |
| `clang` / `llvm-config` / `llvm-link` / `lld` | **均未安装** |
| `kotlinc-native` / `kotlin` / `gradle` | **均未安装** |
| 网络 | apt 经代理可用，可下载 kotlin-native 发行包 |

需要新增的前置依赖：LLVM/clang（供 C 侧与 IR 工具）、Kotlin/Native 发行包（约数百 MB）、可能需要的 JDK 17。

---

## 与存储计划的衔接（已确认：方案 B）

**执行顺序：Kotlin 优先。**

- 改名（K0）→ 自造工具链（K1–K3）→ **直接在 Kotlin 里实现存储栈**（对应 `PLAN-storage-driver.md` 的 P0–P7），**不在 C 侧先做存储栈的参考实现**。
- 现有 C 内核仍作为「可启动基线」保留，直到 K4/K5 逐模块被 Kotlin 替换——所以「C/Kotlin 共存」只发生在**替换既有功能**的过程中；存储栈属于**新增功能**，其代码从第一天就是 Kotlin。
- 后果与应对：
  - 没有 C 参考实现，因此**必须先有可用的 Kotlin 裸机运行时（K3）再动驱动**，否则没有调试手段。
  - `PLAN-storage-driver.md` 的模块划分（mm/pci/driver/block/ata_pio/arkfs/elf）继续有效，但落点由 `kernel/*.c` 变为 `kernel/*.kt`；端口 I/O、MMIO 与中断汇编仍由 `boot/*.S` 与 cinterop 提供，不由 Kotlin 生成裸汇编入口。
  - `.ko` 模块 ABI 保持 C ABI（ET_REL）；模块代码可用 Kotlin 或 C 编写，只要产出符合 ABI 的目标文件。
- 存储栈的实际开工点是 **K3 之后**；K0–K3 期间不碰驱动逻辑。

## 已确认决策汇总

1. 执行顺序：**方案 B（Kotlin 优先）**。
2. 工具链策略：**完全自造 custom target**，含自建 stdlib klib；**不设回退**。
3. 改名范围：代码前缀 `ark_` → `kra_`、宏 `ARK_` → `KRA_`、`include/ark.h` → `include/kra.h`；版本 `0.1.0` → `0.2.0`。
4. K1：作为「硬磕」里程碑推进，不作 Go/No-Go 判定。
5. Worker 线程：实现最小**抢占式**线程基座（静态 TCB + 固定栈池 + 就绪队列 + PIT 时间片），为 KN Worker 提供平台线程；GC 用 noop 以避免额外运行时线程。
6. ASM 最小化：`interrupts.S` 宏化（1574 → ≤100 行）；**32 位代码只允许存在于 `entry_mb2.S`**，新增 64 位汇编只限 `entry_limine.S` / `ctx.S`（+ 可选 `asmio.S`），ASM 总量目标 **≈305–355 行（净减约 80%）**。
7. git：**本仓库不是 git 仓库，且不在全部完成前 `git init`**；`git init` 是 K5 之后的收尾动作。
8. 授权：项目由 MIT 改为 **BSD 3-Clause，Copyright (c) 2026 CryNet Studio by RJ**；清理 GPL（删除 vendored GRUB、裁掉字体许可里的 `debian/*` GPL-2+ 段落）。
9. 引导：**内核完全原生 64 位**；主用 **Limine**（BSD-2-Clause，随 ISO 分发，产物零 GPL），兼容 **multiboot2**（唯一保留的 32 位 trampoline，双镜像靠 `-e` 切换）；**不做 multiboot1、不做 FreeBSD loader**。

---

## 第六部分：KN 裸机重写研究（基于真实源码的结论）

本节把第二部分「可行性边界」落成可执行结论。依据是本地解出的 K/N 源码：目标定义 `org.jetbrains.kotlin.konan.target`（`KonanTarget` / `Family` / `Architecture` / `HostManager` / `Configurables` / `SubTargetProvider`）、属性读取 `TargetProperties.kt`、runtime 模块与构建脚本 `runtime-build.gradle.kts` 与 `platformLibs-build.gradle.kts`、平台后端口 `konan::`（`Porting.h`）。

### 6.1 目标注册不只在 konan.properties

`-Xoverride-konan-properties` **只能覆盖已有目标的属性值**；新增目标还必须在编译器 Kotlin 源码里登记，否则 `HostManager.known()` 直接抛 `Unknown target`。需要动的地方（**已落地**）：

| 位置 | 改动 |
| --- | --- |
| `KonanTarget`（sealed class） | 新增 `object KRAOS_KERNEL_X64 : KonanTarget("kraos_kernel", Family.LINUX, Architecture.X64)` 与 `object KRAOS_X64 : KonanTarget("kraos", Family.LINUX, Architecture.X64)`，并加入 `predefinedTargets` |
| `Family` enum | **不新增**：`Family.LINUX` 本身就是 ELF 家族，两个目标都复用它，以继续共用工具链/sysroot 解析（代价见 6.2） |
| `Architecture` enum | 复用 `X64`，无需改 |
| `HostManager.enabledByHost` | 把两个目标加进 Linux 宿主的可用集合（`commonTargets`），否则默认不可见 |
| `Configurables.kt` | 新增 `KraOSConfigurables : GccConfigurables`，承载 `kraosLinkerScript`/`kraosEntryPoint`/`kraosFreestanding`/`kraosLoadAddress`/`kraosVirtualAddress`/`kraosLinkerExtraLibs`/`kraosExtraLinkerDirs`/`kraosKernelExcludedLibs`/`kraosKraelfBase`/`kraosKraelfMagic` |
| `ConfigurablesImpl.loadConfigurables()` | 新增 `KraOSConfigurablesImpl`；按 `target.name.startsWith("kraos")` **优先分派**，避免落到穷尽 `when (target.family)` 的 `LINUX` 分支 |
| `Linker.kt` | 新增 `KraOSLinker`；`linker()` 分派里 `is KraOSConfigurables` **必须排在 `is GccConfigurables` 之前**（前者是后者子类型） |
| `KraOSLinkerScript.kt`（新增文件） | 内置链接脚本模板；`kraosLinkerScript` 未配置时由编译器写到临时目录（`/tmp/kraos-linker-scripts/`） |
| `TargetProperties.kt` | 属性按 `target.name` 后缀读取，新增 `*.kraos_kernel` / `*.kraos` 条目即可被消费，无需改代码 |

> 结论：**必须 fork K/N 源码并维护补丁**，不能只靠发行版 + 属性覆盖。

### 6.2 konan.properties 的 kraos_kernel / kraos 条目

照着 `linux_x64` 的键写。**注意 host-target 键后缀是 `<host>-<target>`**（如 `linux_x64-kraos_kernel`），
查找不回退到裸 `key.<target>`，漏写会让 `-produce program` 在 `absoluteTargetToolchain` 处抛 NPE。

```
llvmHome.linux_x64-kraos_kernel          = llvm-16.0.0-x86_64-linux-essentials-80
targetToolchain.linux_x64-kraos_kernel   = $gccToolchain.kraos_kernel/x86_64-unknown-linux-gnu
linker.linux_x64-kraos_kernel            = $llvmHome.linux_x64-kraos_kernel/bin/ld.lld

targetTriple.kraos_kernel                = x86_64-unknown-linux-gnu
targetCpu.kraos_kernel                   = x86-64
targetCpuFeatures.kraos_kernel           = +cx8,+fxsr,+mmx,+sse,+sse2,+x87
clangFlags.kraos_kernel                  = -cc1 -emit-obj -disable-llvm-optzns -x ir \
                                           -ffunction-sections -fdata-sections \
                                           -ffreestanding -fno-builtin
clangOptFlags.kraos_kernel               = -O3
clangNooptFlags.kraos_kernel             = -O1
linkerKonanFlags.kraos_kernel            = --defsym __cxa_demangle=Konan_cxa_demangle \
                                           --allow-multiple-definition

kraosFreestanding.kraos_kernel           = true
kraosEntryPoint.kraos_kernel             = Konan_main
kraosLoadAddress.kraos_kernel            = 0x100000
kraosVirtualAddress.kraos_kernel         = 0x0        # 引导期 VMA；见下方「已知限制」
# kraosLinkerScript.kraos_kernel 故意留空 → 用 KraOSLinkerScript 内置默认脚本
```

`kraos`（用户态 KraELF）另有一组键，`kraosFreestanding.kraos = false` 使其退化为 hosted gnu 形状链接，
并预留 `kraosKraelfBase.kraos = 0x400000`、`kraosKraelfMagic.kraos = 0x4b52414f53`。
**KraELF 容器格式与加载器都还没设计/实现**，链接成功时只会打印
`warning: target kraos produces a plain ELF for now; KraELF container generation is not implemented yet.`

> ⚠️ **已知限制（加载地址）**：`kraosVirtualAddress.kraos_kernel = 0x0`、LMA `0x100000`，
> 即内核暂时实际链接在低半区。原因是借用的 glibc 静态归档（`libc.a`/`libpthread.a`）使用
> `R_X86_64_32` 绝对重定位，放不下 `0xffffffff80000000`，链接会报
> `relocation R_X86_64_32 out of range`。等自研运行时位码落地、静态 libc 被摘掉后，
> 把 `kraosVirtualAddress.kraos_kernel` 改回 `0xffffffff80000000` 即可切高半区。
>
> **工具链版本强耦合**：clang 必须用 K/N 自带的那份 LLVM，不能混系统 clang。
> 另外，**不要**把 `-mgeneral-regs-only` 放进 `clangFlags`——K/N ABI 要用 xmm 寄存器。

### 6.3 runtime 后端：模块选择 + 必补的平台符号

runtime 是模块化的（`runtime-build.gradle.kts`），裸机选择：

`custom_alloc` + `noop_gc` + `manual_gcScheduler`（无 GC 线程、无自动 safepoint）+ `noop_crashHandler` + `noop_externalCallsChecker` + `runtime` 主体 + `launcher`。

必须由我们实现的平台后端（`konan::` 命名空间，见 `Porting.h`）：

```
consoleInit / consolePrintf / consoleErrorf / consoleWriteUtf8 / consoleErrorUtf8 /
consoleReadUtf8 / consoleFlush
onThreadExit / isOnThreadExitNotSetOrAlreadyStarted / currentThreadId
getTimeMillis / getTimeMicros / getTimeNanos / getSystemTimeNanos
isLittleEndian
```

外加第三部分那张原语表（线程创建/等待、TLS = `wrmsr IA32_FS_BASE`、mutex/condvar、atomic、映射、abort）。分配器走 `custom_alloc` 接到内核 bump。

**确定「还缺什么」的方法**（替代猜符号）：先用 `kraos_kernel` 编 runtime bitcode + stdlib klib → 与内核目标文件用 `ld.lld` 试链 → 读未定义符号清单 → 在 `runtime/platform/` 逐个补桩 → 迭代到链接通过。这与第四部分 4.1 一致。

### 6.4 构建管线与产物（对齐真实脚本）

- runtime：`compile-to-bitcode`，宏 `KONAN_KRAOS` / `KONAN_X64` / `ELFSIZE=64`；`NO_64BIT_ATOMIC` / `NO_UNALIGNED_ACCESS` 视目标能力决定。
- stdlib：`-Xstdlib-compilation -nostdlib -no-default-libs -Xmanifest-native-targets=...,kraos_kernel`（把 `kraos_kernel` 加进 manifest）。
- 平台 klib：按 family 的 def 文件经 `KonanInteropTask` 生成；需要自带 def（绑定内核 C ABI：`serial_write`、`shell_print`、`panic` 等）。
- 最终链接：kotlinc-native → IR/obj → `llvm-link` + runtime 桩 → `KraOSLinker` 驱动
  `ld.lld --entry=Konan_main -T <链接脚本> --no-dynamic-linker -static -z noexecstack --gc-sections`。
  **已验证产出为 EXEC（无 PT_INTERP）、无 `.interp`/`.dynamic`/`.plt`/`.got` 段的静态 ELF**，
  引导符号 `__kernel_start`/`__bss_start`/`__bss_end`/`__kernel_end` 齐全。
  等 6.3 的 runtime 后端补齐后，再把 `.multiboot2` 头与 `.limine_requests` 段接进来。

### 6.5 先例（可行性证据）

- **Zephyr**：旧版 `HostManager` 存在 `SubTargetProvider` 与 `subTargetProvider.availableSubTarget("zephyr")`，作为**实验性 subtarget** 出现在 `linux_x64` / `macos_x64` 上——证明 JetBrains 曾支持过非 POSIX、无 libc 风格的目标。现代版本里 `SubTargetProvider` 已标注 `@Deprecated("K/N subtargets are removed")`，所以我们要**直接新增 target**，而不是 subtarget。
- **wasm32**：旧版 `enabledByHost` 列表含 `WASM32`，同样是 freestanding 风格目标。

### 6.6 本机环境实测与缺口

| 组件 | 状态 |
| --- | --- |
| gcc 11.4 / ld 2.38 / make / python3 / git | 已有 |
| java | OpenJDK **11**（较新 K/N 需 JDK 17） |
| clang / llvm-config / llvm-link / ld.lld | **缺**（后续已由 K/N 依赖缓存的 llvm-16-essentials 覆盖） |
| kotlinc-native / gradle | **缺**（后续已装 2.1.21 发行版 + 从源码构建编译器 jar） |
| xorriso / qemu-system-x86_64 | **缺**（K0b 与回归测试需要） |

- 选定的 K/N 版本会被**锁死**：自建 stdlib klib 与编译器版本必须严格匹配，之后不随意升级。
- 网络经代理可用时 apt 可补装 LLVM/clang；kotlin-native 发行包与 JDK 17 需另装。

### 6.7 结论

技术上成立、路径清晰，但**成本集中在两处**：(1) fork 并维护 K/N 编译器补丁（新增 `kraos_kernel` / `kraos` 双 target，复用 `Family.LINUX` + 自定义 `KraOSLinker`）；(2) 用「链接期未定义符号」驱动的方式补齐 runtime 平台后端。两者都没有捷径，符合第二部分「硬磕到底、不设回退」的既定决策。

**当前进度对照**：

| 项 | 状态 |
| --- | --- |
| 双 target 注册 + `KraOSConfigurables` + `KraOSLinker` + 默认链接脚本 | ✅ 已实现并验证（`kraos_kernel` 出 EXEC、无 PT_INTERP 的静态 ELF） |
| 自研运行时位码（替掉借用的 linux_x64 `*.bc`） | ❌ 未做（glibc 归档已摘，但 K/N 位码仍借用 linux_x64 的 `*.bc`，`libstdc++` 仍链接） |
| 加载地址切高半区 `0xffffffff80000000` | ❌ 被 `R_X86_64_32` 绝对重定位卡住，等运行时落地后再切 |
| KraELF 容器格式 + `kraos` 加载器 | ❌ 未设计、未实现（`kraos` 目前是占位目标） |
| K0 改名 / K0b ISO（xorriso + Limine）/ qemu 回归 | ❌ 未做 |