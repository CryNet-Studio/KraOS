# HANDOFF — KraOS / Kotlin-Native「新增 kraos_kernel + kraos 双目标」阶段交接

> 生成时间：2026-10-02
> 交接范围：本次会话全部产出（代码、脚本、属性、验证证据、阻塞点、后续计划）
> 重要约束：**在项目彻底完成前不要 `git init`**（用户明确要求）。

---

## 0. 一句话状态

已把 `KraOS` 定义为 **基于 Kotlin/Native（Konan）新增的两个自定义目标**（不再自造编译器 `kraoc`）：

| 目标 | 定位 | 状态 |
|---|---|---|
| `kraos_kernel` | freestanding 裸机内核（无 libc 启动、无 PT_INTERP 的静态 ELF） | **已实现、已验证** |
| `kraos` | 用户态 KraELF 容器目标 | **仅占位**：目标能注册、能出 ELF，容器格式与加载器**尚未设计/实现** |

> **【2026-10-03 更新】工具链路线已切换为「直接改 K/N 源码、从源码构建」**，取代旧的 jar 手术方案。
> 全量 v2.1.21 单仓库源码在 `.tooling/kotlin-src/`；改动固化为
> [`toolchain/konan-patch/kraos-targets.patch`](toolchain/konan-patch/kraos-targets.patch)；
> 构建脚本 [`toolchain/konan-patch/build-kn.sh`](toolchain/konan-patch/build-kn.sh)。
> **本轮已从源码成功构建 `kotlin-native-compiler-embeddable-2.1.255-SNAPSHOT.jar`（267MB），
> 并经运行时代码路径验证两个目标已注册**（`predefinedTargets.size` 由 21→23，
> `HostManager.targetByName("kraos_kernel")` / `("kraos")` → `family=LINUX arch=X64`）。

> **【2026-10-04 更新】单目标已拆分为 `kraos_kernel` + `kraos` 双目标，并新增 `KraOSLinker`。**
> `kraos_kernel` 走新的 `KraOSLinker`，**完全不走 gnu 启动约定**（不引 crt*、不加
> `-dynamic-linker`/`--sysroot`/`-export-dynamic`/`--eh-frame-hdr`），产物已实测为
> **EXEC（无 PT_INTERP）、无 `.interp`/`.dynamic`/`.plt`/`.got` 段**的静态 ELF。
> 端到端验证：`konanc -target kraos_kernel hello.kt -o hello_kernel` → `hello_kernel.kexe`
> （`.text` 落在 `0x100000`，入口 `0x128180`，`__kernel_start`/`__bss_start`/`__bss_end`/
> `__kernel_end`/`Konan_main` 符号齐全）；`-p library` 也能正常出 klib。细节见 §8.6。

> **【2026-10-04 晚 更新】内核已经真的跑起来了 —— 见 §11。**
> multiboot2 引导 → 32 位 → 长模式 → SSE 初始化 → 系统调用门 → FS 基址 → `Konan_main`，
> K/N 运行时开始执行并一路走到 `ThreadLocalStorage` 初始化；异常被自建 IDT 捕获并
> 在串口上打出向量号/错误码/RIP/CR2/全部寄存器/现场内存。
> `make check` 四条断言全绿（无 PT_INTERP / 入口链 / 无动态段 / 无 ifunc 桩）。

当前进度：**P2 已完成并验机** —— Kotlin 横幅与物理页分配器自检都在串口上打出
`KraOS boot ok mem=…MiB cpu=…`。三件事全部落地：
1. 基于 multiboot2 内存图的物理页帧分配器（§12.1）；
2. glibc 静态归档（`-lc/-lpthread/-ldl/-lm`）全部摘掉，`boot/libc_min.c` 补齐引用面（§12.2）；
3. 内核主体切回高半区 `0xffffffff80000000` 并能启动（§12.3，本轮收尾）。
引导矩阵 6/6 + `make check` 5/5 全绿。剩余工作：自研运行时位码 + KraELF 格式与
加载器 + stdlib 平台 actual（§6.3–6.5），以及 §12.4 列的欠账。

---

## 1. 关键决策（必须延续，勿回退）

1. **不自造编译器**：放弃 `toolchain/kraoc/`（Python 自造编译器尝试），改为 **patch 官方 Kotlin/Native 发行版**新增目标。
2. **内核完全 64 位**，不做 32 位目标/兼容；仅允许 **multiboot2 启动时从 32 位保护模式快速切入 64 位**。
3. **引导器用 Limine 取代 GNU GRUB**。
4. **许可全面 BSD 3-Clause**（CryNet Studio by RJ），清理 GPL。
5. **系统全面改名 KraOS**（代码/文档/配置），原 ArkOS 0.1.0 / 0.5.0 为参照与改写基础。
6. **不 `git init`** 直到彻底完成。

---

## 2. 本次会话产出清单（我写的 / 改的）

### 2.1 Konan 目标补丁（核心）
目录：`KraOS/toolchain/konan-patch/`

单目标已拆为**双目标**，补丁改名重写为 [`kraos-targets.patch`](toolchain/konan-patch/kraos-targets.patch)
（616 行，7 个文件）：

| 文件 | 作用 |
|---|---|
| `native/utils/.../target/KonanTarget.kt` | 新增 `object KRAOS_X64 : KonanTarget("kraos", Family.LINUX, Architecture.X64)` 与 `object KRAOS_KERNEL_X64 : KonanTarget("kraos_kernel", Family.LINUX, Architecture.X64)`，并加入 `predefinedTargets`。 |
| `native/utils/.../target/HostManager.kt` | 把两个目标加入 `commonTargets`，使 Linux 宿主上 `enabled` 可见。 |
| `native/utils/.../target/Configurables.kt` | 新增接口 `KraOSConfigurables : GccConfigurables`，属性：`kraosLinkerScript`/`kraosEntryPoint`/`kraosFreestanding`/`kraosLoadAddress`/`kraosVirtualAddress`/`kraosLinkerExtraLibs`/`kraosExtraLinkerDirs`/`kraosKernelExcludedLibs`/`kraosKraelfBase`/`kraosKraelfMagic`。 |
| `native/utils/.../target/ConfigurablesImpl.kt` | 新增 `KraOSConfigurablesImpl`；`loadConfigurables()` 按 `target.name.startsWith("kraos")` 优先分派。 |
| `native/utils/.../target/Linker.kt` | 新增 `KraOSLinker`（见 2.1.1）；`linker()` 分派里 `is KraOSConfigurables` **必须排在 `is GccConfigurables` 之前**（前者是后者子类型）。 |
| `native/utils/.../target/KraOSLinkerScript.kt` | **新增文件**：内置链接脚本模板。 |
| `kotlin-native/konan/konan.properties` | 双目标的属性块（freestanding 内核 + KraELF 占位）。 |

> 旧的 `kn-patch/`（jar 手术方案，含 `apply.sh`/`patch_jar.py` 与单目标属性文件）
> 已整体废弃，仅作存档（见 §10）。

#### 2.1.1 `KraOSLinker`（裸机链接器，已实现）

- `kraos_kernel`（`kraosFreestanding=true`）**完全不走 gnu 启动约定**：
  不引入 `crt1.o`/`crti.o`/`crtbegin.o`/`crtend.o`/`crtn.o`，不加 `-dynamic-linker`、
  `--sysroot`、`-export-dynamic`、`--eh-frame-hdr`。
- 链接行形如：
  `ld.lld --entry=Konan_main -T <脚本> --no-dynamic-linker -static -z noexecstack --gc-sections --build-id=sha1 ...`
- 入口符号 `Konan_main`（K/N 运行时的 main，位于 `launcher.bc`）；引导代码设好栈后跳它。
- 默认链接脚本由编译器现写到临时文件（`/tmp/kraos-linker-scripts/kraos-kernel.ld`），
  可用 konan.properties 的 `kraosLinkerScript` 属性覆盖成自己的脚本。
- 静态库仍走 `staticGnuArCommands`；动态库直接 `error`（KraOS 不支持动态库）；sanitizer 不支持。
- `kraos`（`kraosFreestanding=false`）退化为 hosted gnu 形状链接，链接时打印
  `warning: target kraos produces a plain ELF for now; KraELF container generation is not implemented yet.`

### 2.2 对发行版做的两处「运行时环境」改造（在 `.tooling` 里，zip 未含大文件）
> 这些改造是**为了打通链路临时打的**，现已由 `deploy-to-dist.sh` 自动化（见 6.2）。

1. **stdlib 解析**：把 `klib/common/stdlib` 复制为
   `klib/platform/{kraos,kraos_kernel}/stdlib`，并把其 `default/manifest` 中
   `native_targets=` 追加对应目标名。
   → 解决 `error: KLIB resolver: Could not find "stdlib"`。
2. **原生运行时位码（bootstrap）**：把 `konan/targets/linux_x64/native/*.bc`（30 个）
   复制为 `konan/targets/{kraos,kraos_kernel}/native/`。
   → 解决 `Error parsing .../konan/targets/<target>/native/compiler_interface.bc`。
   ⚠️ 这是**临时的**，等价于复用 Linux 运行时；裸机阶段必须换成 KraOS 自研运行时。

### 2.3 供移植参考的源码/资料（会话中提取）
- `/workspace/targets/`：Konan target 层源码提取（`KonanTarget.kt`、`HostManager.kt`、
  `TargetManager.kt`、`Configurables.kt`、`Family.kt`、`Architecture.kt`、`KonanProperties.kt` 等）。
- `/workspace/rt/`：K/N 运行时/平台库的 C++ 源码与构建文件清单（Allocator、Thread、
  Exceptions、Memory、PthreadUtils、`runtime-build.gradle.kts`、`platformLibs-build.gradle.kts` 等），
  以及 `RUNTIME_README.md` / `PLATFORMLIBS_README.md`。
- `/workspace/old1430/`：旧版（1.4.30）target 层对照文件。
- `/workspace/poc/hello.kt`：PoC 用例。
- `/workspace/.tooling/kotlin-src/`：稀疏克隆的 Kotlin/Native 源码（含
  `native/utils/src/org/jetbrains/kotlin/konan/target/Linker.kt` 等，**移植 linker 时必看**）。

### 2.4 被修改的 ArkOS 工程
- `ArkOS/ArkOS-0.1.0/`（原 0.1.0 工程，含 `toolchain/kn-patch`）。
- `upstream/ArkOS-0.5.0/`（0.5.0 参照工程，含 `boot/`、`kernel/`、`docs/`、`user/`、`sdk/` 等）。
- `ArkOS-backup-pre-k0/`（K0 改名前的备份：`pre-limine/`、`_limine-src/`、`_limine-bin/`、`verification/`）。

---

## 3. 环境与工具链（沙箱内，zip 未含下载物）

| 组件 | 路径 | 说明 |
|---|---|---|
| Kotlin/Native 2.1.21 发行版 | `/workspace/.tooling/kotlin-native-linux-x86_64-2.1.21` | `konanc` 入口 `bin/konanc`。**勿打包**（371M）。 |
| 编译器 jar | `.../konan/lib/kotlin-native-compiler-embeddable.jar` | 已 patch；原始备份 `...jar.orig`。 |
| JVM kotlinc（用于编译补丁类） | `/workspace/.tooling/kotlinc/bin/kotlinc` | **勿打包**（91M）。 |
| K/N 源码（参考） | `/workspace/.tooling/kotlin-src` | **勿打包**（54M）。 |
| 系统 LLVM 14 | `/usr/lib/llvm-14` | 版本不兼容 K/N（K/N 2.1.21 绑定 LLVM 16）。 |
| LLVM 16 | `~/.konan/dependencies/llvm-16.0.0-x86_64-linux-essentials-80` | 已由源码构建过程自动下载，codegen 可用（见 8.5）。 |

**重要事实**：K/N 2.1.21 强绑定 **LLVM 16**（`llvmVersion.linux_x64 = 16`）。
本沙箱 **JetBrains 依赖镜像取不到该 LLVM 包**（`download.jetbrains.com` 与
`download-cdn.jetbrains.com(.cn)` 对该 object 均返回 404）；
apt.llvm.org 源的安装曾中途被打断，最终由源码构建过程自动下载的
llvm-16-essentials bundle 覆盖（见 8.5）。

---

## 4. 复现步骤（当前可复现部分的完整命令）

```bash
KN=/workspace/.tooling/kotlin-native-linux-x86_64-2.1.21

# 1) 一键部署（源码构建的 jar → 官方发行版；幂等，双目标循环）
KraOS/toolchain/konan-patch/deploy-to-dist.sh $KN

# 2) 确认两个目标都可见
$KN/bin/konanc -list-targets        # 应同时包含 kraos_kernel 与 kraos

# 3) freestanding 内核：出静态、无 PT_INTERP 的 ELF
$KN/bin/konanc -target kraos_kernel hello.kt -o /tmp/hello_kernel
# => /tmp/hello_kernel.kexe
readelf -h /tmp/hello_kernel.kexe   # Type: EXEC，Entry: 0x128180
readelf -l /tmp/hello_kernel.kexe   # PT_INTERP 数量为 0

# 4) 出 klib（已验证可成功）
$KN/bin/konanc -target kraos_kernel -produce library hello.kt -o /tmp/hello_kernel_lib

# 5) 用户态占位目标（当前只出普通 ELF，并打印 KraELF 未实现警告）
$KN/bin/konanc -target kraos hello.kt -o /tmp/hello_user
# => /tmp/hello_user.kexe（EXEC，Entry 0x428180，无 PT_INTERP）
```

**注意**：`-Xoverride-konan-properties` 只接受**内联 `key=value;key=value`**，
不接受文件路径（传路径会报 `incorrect property format`）。属性现已由
`deploy-to-dist.sh` 直接并入 `$KN/konan/konan.properties`（更干净，见 6.2）。

---

## 5. 当前阻塞点（按优先级）

### 5.1 【P0】LLVM 16 未装完 → codegen 无法进行
- 现象（未装 LLVM16 前，走全程序编译）：
  依次经过 stdlib 解析修复、runtime 位码补齐后，下一步会调用 `clang` 编译 IR，
  但 K/N 生成的 LLVM 16 bitcode 必须由 **LLVM 16** 解析，系统只有 LLVM 14。
- 处理：添加 apt.llvm.org 源后
  `apt-get install -y --no-install-recommends clang-16 lld-16 libllvm16 llvm-16`
  （后续已由源码构建过程自动下载的 LLVM 16 bundle 覆盖，见 8.5）。
- 属性侧对应键已改为 `llvmHome.linux_x64-{kraos,kraos_kernel}`、
  `linker.linux_x64-{kraos,kraos_kernel}`。

### 5.2 【P1】运行时位码仍不是裸机的（KraOSLinker 已解决 linker 这一半）
- **linker（已解决）**：`KraOSLinker` 已落地并接管两个目标的链接
  （`native/utils/.../Linker.kt`），freestanding 路径不再引入 crt* 与动态链接器。
- **运行时（未解决）**：`konan/targets/{kraos,kraos_kernel}/native/*.bc` 仍是 linux_x64 的副本，
  内含对 libc/pthread/mmap 的依赖 → `kraos_kernel` 现在虽然出的是无 PT_INTERP 的静态 ELF，
  但里面仍有 glibc/pthread 符号。
  → 必须换自研运行时位码（见 6.4），并同步清掉 `kraosKernelExcludedLibs` /
  `kraosLinkerExtraLibs` 里多余的 `-l`。

### 5.3 【P2】`kraos_kernel` 加载地址暂放低地址（VMA `0x0` / LMA `0x100000`）
- 现象：想按常规把内核链到 `0xffffffff80000000` 高半区，但链接直接失败：
  `relocation R_X86_64_32 out of range`。
- 原因：借用的 glibc 静态归档（`libc.a`/`libpthread.a`）使用 `R_X86_64_32` **绝对重定位**，
  放不下高半区。
- 现状：引导期把 VMA 暂设 `0x0`、LMA `0x100000`（即实际链接在 `0x100000`），
  靠链接脚本把 `.bss` 之后的段留在低地址，`__kernel_start = 0x100000`。
- 切换条件：自研运行时位码落地、静态 libc 被摘掉之后，把
  `kraosVirtualAddress.kraos_kernel` 改回 `0xffffffff80000000` 即可切高半区。
  **这是已知限制，不是最终形态。**

### 5.4 【P3】KraELF（`kraos` 目标）格式与加载器尚未设计
- `kraosKraelfBase.kraos = 0x400000`、`kraosKraelfMagic.kraos = 0x4b52414f53` 属性已预留，
  但容器布局、加载器都还没设计，`kraos` 目前只是 hosted gnu 形状的占位目标
  （链接时打印 `KraELF container generation is not implemented yet.` 警告）。

### 5.5 【P3】stdlib 仍是「借来的」
`klib/platform/{kraos,kraos_kernel}/stdlib` 只是把 common 多平台 stdlib 的 target 列表加了
对应目标名，其 platform `actual`（内存/异常/输出等）并未针对 KraOS 实现。

---

## 6. 下一步计划（建议顺序）

### 6.1 端到端复验 LLVM 16 + 双目标链路（P0）
```bash
ls /usr/lib/llvm-16/bin/clang /usr/lib/llvm-16/bin/ld.lld   # 校验
# 然后重跑（属性已由 deploy-to-dist.sh 并进 konan.properties）：
$KN/bin/konanc -target kraos_kernel hello.kt -o /tmp/hello_kernel
readelf -h /tmp/hello_kernel.kexe; readelf -l /tmp/hello_kernel.kexe
```
> 说明：这一步验证的是「freestanding 链接链路完整」（EXEC、无 PT_INTERP）；
> ELF 内仍有 glibc/pthread 符号，运行时裸机化在 6.4。

### 6.2 保持 `deploy-to-dist.sh` 的双目标部署（P0，消除手工步骤）
脚本已改成对 `kraos` / `kraos_kernel` 双目标循环：
1. **属性注入**：从 `kraos-targets.patch` 抽出 `+` 行属性块，幂等地插进
   `$KONAN_HOME/konan/konan.properties`（插在 `compilerVersion=` 注释前）。
   先清掉早期单目标合并留下的属性块，避免留下一堆无人引用的 host-target 键。
2. **stdlib**：`rm -rf klib/platform/<target>/stdlib && cp -a klib/common/stdlib <该路径>`，
   再用 python 在 `default/manifest` 的 `native_targets` 追加目标名。
3. **运行时 bootstrap**：`cp -a konan/targets/linux_x64/native konan/targets/<target>/native`
   （裸机运行时做好后替换为真实产物）。
4. **清理遗留**：顺手删掉早期单目标时代留下的部署物
   （`konan/targets/<旧目标名>`、`klib/platform/<旧目标名>`）与 properties 里的旧块。
5. 全程幂等（`jar.orig` 只备份一次，标记段先删再加）；末尾校验
   `bin/konanc -list-targets` 须同时出现 `kraos` 与 `kraos_kernel`。

### 6.3 KraELF 格式与加载器（P1，尚未开始）
- `kraos` 目标目前只是占位：`kraosFreestanding.kraos = false`，退化 hosted gnu 形状，
  链接时打印 `KraELF container generation is not implemented yet.`。
- 待定：KraELF 容器头部布局（已预留 `kraosKraelfMagic = 0x4b52414f53`、
  `kraosKraelfBase = 0x400000`）、段/重定位承载方式、以及内核侧加载器。
- 落地后把 `finalLinkCommands()` 里 `!kraosFreestanding` 的占位分支换成真正的容器生成。

### 6.4 构建 KraOS 自研 K/N 运行时位码（P1，工作量最大）
- 目标：把 `konan/targets/kraos_kernel/native/*.bc` 换成针对裸机编译的运行时。
- 参考源码：`/workspace/rt/`（Allocator/Thread/Exceptions/Memory/… 的 C++ 与构建文件）、
  K/N 的 `kotlin-native/runtime/` 与 `platformLibs/`。
- 需要为 KraOS 提供的最小系统调用桩（建议先做成内核侧服务，再让运行时回调）：
  - 内存：页分配/mmap 等价物（给 GC/allocator）
  - 线程：创建/切换/TLS（给 Worker，见 6.6）
  - 异常：栈展开/unwind 支撑
  - 输出：控制台/串口（`println` 落地）
- 落地后连带清理属性：清空 `kraosKernelExcludedLibs`、
  收敛 `kraosLinkerExtraLibs`（去掉 `-lstdc++/-lpthread/-ldl/-lc/-lm`），
  并把 `kraosVirtualAddress.kraos_kernel` 改回 `0xffffffff80000000` 切高半区（见 5.3）。

### 6.5 stdlib 平台 actual 补全（P2）
在 6.4 的服务之上，为 `kraos_kernel` 提供 stdlib/platform 的 `actual` 实现，
产出**真正属于 kraos_kernel 的 stdlib klib**，替换 5.5 的借用版本。

### 6.6 K/N Worker 线程基座（P2，用户早前提过）
实现最小抢占式线程：TCB、就绪队列、轮转调度、TLS；与 6.4 的线程桩对接。

### 6.7 K0 改名 & K0b 引导（P2/P3）
- **K0**：全面把 `ArkOS` 字样改为 `KraOS`（代码/文档/配置/构建），注意
  `third_party/grub/**` 里的历史文案也要处理（用户已明确要求）。
- **K0b**：用 `xorriso` + Limine 生成 ISO，做 **Limine + multiboot2 双镜像**；
  入口仅保留 multiboot2 所必需的 32→64 位 trampoline。
- **qemu**：装 `qemu-system-x86_64`，让 6.4 的产物真正在 QEMU 里跑起来回归。

---

## 7. 打包说明（本 zip 的纳入/排除）

**纳入**：本项目全部自写/修改的源码与脚本、文档、目标层与运行时参考资料、
两个 ArkOS 工程（0.1.0 / 0.5.0，**已剔除镜像**）、K0 前备份、根目录研究产物
（`tree.json`、`k_v*.json`、`t_v*.properties` 等）。

**排除**（体积大/可重下/可复现，均已在本文档标注来源）：
- `.tooling/kotlin-native-linux-x86_64-2.1.21/`（发行版 371M，可由官方 tar 解压 + `deploy-to-dist.sh` 复现）
- `.tooling/kotlin-src/`（上游源码克隆 54M）、`.tooling/kotlinc/`（91M）
- `.tooling/*.tar.gz`、`.tooling/*.zip`、`.uploads/*.zip`、`.screenshots/`
- 各 `*.iso` / `*.img` / `*.tar.xz`（构建产物与第三方压缩包）

> 若要「带打补丁后的编译器」的完整环境，需另行重跑第 4 节步骤（约几分钟，前提是本机有 LLVM 16）。

---

## 8. 验证证据（本次已确认）

| 项 | 命令 | 结果 |
|---|---|---|
| 目标可见 | `konanc -list-targets` | 列表末尾出现 `kraos_kernel` 与 `kraos` ✅ |
| 目标补丁生效 | `konanc -target kraos_kernel -produce library hello.kt` | 产出 klib ✅ |
| stdlib 解析 | 同上（未修 manifest 前） | `Could not find "stdlib"` → 修复后消失 ✅ |
| 运行时位码 | 全程序编译（未装 LLVM16 前） | `compiler_interface.bc` 缺失 → 补位码后越过 ✅ |

### 8.5 源码构建路线的验证证据（2026-10-03）

| 项 | 方法 | 结果 |
|---|---|---|
| 从源码构建编译器 jar | `toolchain/konan-patch/build-kn.sh`（含 `-Pkotlin.native.enabled=true` + 强制 IPv4 + 抗抖动重试） | `BUILD SUCCESSFUL in 9m 6s`，348 任务执行 ✅ |
| 产物 | `kotlin-native/prepare/kotlin-native-compiler-embeddable/build/libs/kotlin-native-compiler-embeddable-2.1.255-SNAPSHOT.jar` | 267MB ✅ |
| 源码改动编入 jar | 反编译校验 | `KonanTarget.class`/`HostManager.class` 各引用 `KRAOS_KERNEL_X64` 与 `KRAOS_X64`；两个 `KonanTarget$*.class` 均含 `kraos_kernel`/`kraos`+`LINUX` 字面量 ✅ |
| **运行时注册验证** | Java harness 直连胖 jar，走真实 `KonanTarget.predefinedTargets`/`HostManager.targetByName` 代码路径 | `predefinedTargets.size` 21→23；`targetByName("kraos_kernel")`/`("kraos")` → `family=LINUX arch=X64` ✅ |
| **发行版获取** | `download.jetbrains.com/kotlin/native/builds/releases/2.1.21/linux-x86_64/...tar.gz`（重定向 download-cdn.jetbrains.com.cn，**无需 github 镜像**） | 174MB `kotlin-native-linux-x86_64-2.1.21.tar.gz` ✅ |
| **dist 部署（纯净验证）** | `deploy-to-dist.sh <dist>`（换 jar + 并双目标属性块 + stdlib/bootstrap + 位码拷贝 + 清遗留，幂等） | 5 步全过 ✅ |
| **端到端：目标注册** | `bin/konanc -list-targets` | 末尾同时出现 `kraos_kernel` 与 `kraos` ✅ |
| **端到端：klib** | `konanc -target kraos_kernel -produce library hello.kt` | klib 正常产出，manifest `native_targets` 含该目标 ✅ |

源码构建期间自动下载并使用了 **LLVM 16**（`llvm-16.0.0-x86_64-linux-dev-80.tar.gz`，1GB，走 cache-redirector）——这顺带覆盖了 §5.1 的 LLVM 依赖，本机从此有可用 LLVM 16 工具链。
`~/.konan/dependencies/` 已备好 llvm-16-essentials + gcc-8.3 sysroot 两个 bundle（共 8.6G 依赖缓存），两个目标的属性直接引用同名 bundle 无需再下。

### 8.6 freestanding 链接形状的验证证据（2026-10-04）

`konanc -target kraos_kernel hello.kt -o hello_kernel`：

| 检查项 | 方法 | 结果 |
|---|---|---|
| 产物名 | `ls` | `hello_kernel.kexe`（K/N 给可执行文件加 `.kexe` 后缀）✅ |
| ELF 类型 | `readelf -h` | **EXEC（可执行文件，非 PIE）**，入口 `0x128180` ✅ |
| 无解释器 | `readelf -l` | **PT_INTERP 数量为 0** ✅ |
| 无动态链接段 | `readelf -S` | 无 `.interp`/`.dynamic`/`.plt`/`.got` 段 ✅ |
| 段布局 | 同上 | `.text` 落在 `0x100000`，`.bss` 为 NOBITS ✅ |
| 引导符号 | `readelf -s` | `__kernel_start=0x100000`、`__bss_start`、`__bss_end`、`__kernel_end`、`Konan_main` 齐全 ✅ |
| klib 路径 | `konanc -target kraos_kernel -p library hello.kt` | 正常产出 ✅ |
| 用户态占位目标 | `konanc -target kraos hello.kt -o hello_user` | `hello_user.kexe`，EXEC，入口 `0x428180`，无 PT_INTERP；同时打印 KraELF 未实现警告 ✅ |

链接行（`ld.lld`）：
`--entry=Konan_main -T /tmp/kraos-linker-scripts/kraos-kernel.ld --no-dynamic-linker -static -z noexecstack --gc-sections --build-id=sha1 ...`

> ⚠️ **已知限制（见 5.3）**：当前 VMA `0x0` / LMA `0x100000`，未能切到高半区；
> ELF 内仍含 glibc/pthread 符号（运行时位码仍是 linux_x64 的副本）。

---

## 9. 注意事项 / 雷区

- ❌ 不要 `git init`（用户硬性要求）。
- ❌ 不要再回退到 jar 手术（`kn-patch/` 已废弃）；改动一律落到 `.tooling/kotlin-src/` 源码。
- ⚠️ `.tooling/kotlin-src/` 不是 git 仓库；**从 tarball 重解会覆盖全部改动**，重解后必须
  `patch -p1 < toolchain/konan-patch/kraos-targets.patch` 重新应用。
- ⚠️ `:kotlin-native:*` 工程由 `-Pkotlin.native.enabled=true` 守卫，缺了报 "project 'kotlin-native' not found"。
- ⚠️ 本机网络两大硬约束（见 build-kn.sh 注释）：**IPv6 不可用**（须 `-Djava.net.preferIPv4Stack=true`）、
  网络抖动频繁（已放宽 Gradle 重试/超时）。
- ⚠️ K/N 构建逻辑要 JDK 8 toolchain；本机无 sudo，JDK 8 已解包到 `.tooling/jdk`（`setup-jdk8.sh` 可重放）。
- ⚠️ 构建产物版本号是 `2.1.255-SNAPSHOT`（单仓库 dev 版号），但源码内容=2.1.21；
  部署时把 jar 换进 2.1.21 发行版 `konan/lib/`，并把 kraos 双目标属性块并进该发行版的 `konan/konan.properties`。
- ❌ 不要把 `-mgeneral-regs-only` 放进 `clangFlags`（K/N ABI 要用 xmm 寄存器）——两个目标的属性块均已据此取舍。
- ⚠️ **两个目标都保留 `Family.LINUX`**，目的是复用工具链/sysroot 解析；
  真正的差异全部收敛在 konan.properties 与 `KraOSLinker` 里。新增属性入口走
  `loadConfigurables()` 的 `target.name.startsWith("kraos")` 优先分派。
- ⚠️ **`linker()` 分派顺序不能动**：`is KraOSConfigurables` 必须排在 `is GccConfigurables` 之前，
  因为 `KraOSConfigurables` 是后者的子类型，顺序反了会退回 `GccBasedLinker`（重新引入 crt*）。
- ⚠️ 工具链侧仍是 **bootstrap 占位**：借用了 linux_x64 同款 llvm-16-essentials + gcc-8.3/sysroot bundle
  （`~/.konan/dependencies/...`，非宿主路径）。`kraos_kernel` 的链接形状已经正确，但 ELF 内容仍依赖 libc；
  自研运行时落地后需删 toolchain/sysroot/crt 条目并收敛 `kraosLinkerExtraLibs`（见 6.4）。
- ⚠️ **加载地址暂放低地址**：`kraosVirtualAddress.kraos_kernel = 0x0`、
  `kraosLoadAddress.kraos_kernel = 0x100000`。原因与切回高半区的条件见 5.3。
- ⚠️ `kraos` 目标目前是**占位**：KraELF 容器格式与加载器都还没设计/实现，
  链接时会打印 `warning: target kraos produces a plain ELF for now; ...`，别把它当成已完成的功能。
- ⚠️ **hostTargetString 键后缀顺序是 `<host>-<target>`**（`linux_x64-kraos_kernel`，
  不是 `kraos_kernel-linux_x64`），且查找只落到 `key.<host>-<target>` → 裸 key，
  **不回退** `key.<target>`（`compiler/util-io/.../properties/Properties.kt:51`）。
  `llvmHome`/`targetToolchain`/`linker`/`linkerHostSpecificFlags`/`dependencies`
  这类 host-target 键漏写会导致 `-produce program` 在
  `ClangArgs.<init>` → `absoluteTargetToolchain` 抛 NPE（本轮踩实）。
- ⚠️ K/N dist 不走 github：用 `download.jetbrains.com/kotlin/native/builds/releases/`
  （重定向到 `download-cdn.jetbrains.com.cn`，本机可达，174MB）。

---

## 10. 关键文件索引

**当前路线（源码构建 + 发行版部署）**
- 目标补丁（可复制移植源）：`KraOS/toolchain/konan-patch/kraos-targets.patch`（`patch -p1` 应用于 `.tooling/kotlin-src/`）
- 构建脚本：`KraOS/toolchain/konan-patch/build-kn.sh`、`setup-jdk8.sh`
- 部署脚本：`KraOS/toolchain/konan-patch/deploy-to-dist.sh`（幂等；换 jar + 并双目标属性块 + stdlib + 位码 + 清遗留 + 验证）
- 说明：`KraOS/toolchain/konan-patch/README.md`
- 源码改动位置（补丁落点）：
  `.tooling/kotlin-src/native/utils/src/org/jetbrains/kotlin/konan/target/{KonanTarget.kt,HostManager.kt,Configurables.kt,ConfigurablesImpl.kt,Linker.kt,KraOSLinkerScript.kt}`、
  `.tooling/kotlin-src/kotlin-native/konan/konan.properties`
- 产物 jar：`.tooling/kotlin-src/kotlin-native/prepare/kotlin-native-compiler-embeddable/build/libs/kotlin-native-compiler-embeddable-2.1.255-SNAPSHOT.jar`
- **工作发行版**：`.tooling/kotlin-native-linux-x86_64-2.1.21/`（已部署 kraos_kernel + kraos；原 tarball 同目录保留；`bin/konanc -target kraos_kernel` 可用）
- 验证用最小 konan home：`.tooling/konan-home/`（konan.properties + trove4j.jar + env_blacklist）

**移植参考**
- linker 移植参考：`.tooling/kotlin-src/native/utils/src/org/jetbrains/kotlin/konan/target/Linker.kt`
  （`GccBasedLinker`≈L363、`linker()`≈L511，v2.1.21）
- 计划文档：`KraOS/docs/`（PLAN-kraos-kotlin-native.md 等）
- 工程：`KraOS/`（kernel/include/boot/scripts/tests/toolchain/docs）
- **裸机调试工具（本机无 gdb，直接讲 QEMU 的 RSP 远程协议）**：
  `KraOS/tools/gdbwatch.py`（下断点并逐次打印寄存器）、
  `KraOS/tools/bpwalk.py`（在一串地址上依次停）、
  `KraOS/tools/bpmulti.py`（同时下多个断点计数）、
  `KraOS/tools/newtrace.py`（跟踪 `operator new` 的入参与返回值）。
  用法与踩过的坑见各文件头注释，以及 §11.4。

**已废弃（仅存档）**
- jar 手术方案：`KraOS/toolchain/kn-patch/`（见 `DEPRECATED.md`）

---

## 11. 引导阶段：从 multiboot2 到 Kotlin 代码在裸机上执行

> 本节是「开始建造 KraOS_Kernel 内核」这一任务的全部产出。
> 前面 §2–§10 描述的是**编译器和链接器**阶段（双目标注册、freestanding 链接形状）；
> 本节是**跑起来**的阶段。

### 11.1 现在的状态（一句话）

**P1 已完成。** 内核在 QEMU 里完成 multiboot2 引导、切入长模式、开启 SSE 与系统调用，
进入 `Konan_main`，K/N 运行时完整初始化，Kotlin 代码在裸机上真实执行，
并打印出验收横幅：

```text
[boot] Multiboot2 入口，正在准备 x86_64 长模式
[boot] long mode 可用
[boot] 已开启 PAE/LME/PG
[boot] 已进入 64 位长模式
[boot] 移交 Konan_main
[kraos] ===== KraOS 内核启动 =====
[kraos] multiboot2 magic = 0x0000000036d76289  ✓
[kraos] 内存区数量 = 7
        base=0x0000000000000000 len=0x000000000009fc00 type=1 (可用)
        base=0x0000000000100000 len=0x000000000fee0000 type=1 (可用)
        …
[kraos] libc 垫片 brk=0x0000000001421000 mmap 探针=0x00000000034a0000
[kraos] 引导堆已分配 8 次 / 1028 KiB
[kraos] Kotlin/Native 内核入口就绪；P1 引导骨架完成

KraOS boot ok  mem=255 MiB  cpu=1

[kraos] 内核 main 返回，停机
```

`cpu` 随 `-smp` 正确变化（已验证 `-smp 1/2/4/8` 分别报 1/2/4/8）。
干净重建 23.5 秒。

### 11.2 新增/改动的文件

| 文件 | 行数 | 作用 |
|---|---|---|
| `boot/entry.S` | 747 | 32 位 multiboot2 入口 → 长模式；页表、GDT、SSE 初始化、IDT 桩与故障处理器、系统调用入口、FS 基址、串口、64 MiB 堆 |
| `boot/libc_shim.c` | 508 | 堆与 `brk`/`sbrk`/`mmap`/`munmap`/`mprotect`；系统调用分发；IRELATIVE 重定位应用；**IDT 构建**；故障诊断打印 |
| `boot/ifunc_shim.c` | 459 | 35 个字符串/内存函数的自研实现 + `__` 前缀隐藏别名，供 `--wrap` 消解 glibc ifunc |
| `boot/linker.ld` | — | 段布局、`.rela.iplt` 输出节、`PT_TLS` |
| `kernel/src/**.kt` | — | Kotlin 侧：multiboot2 解析、启动堆、自检横幅 |
| `Makefile` | — | `check` 改为**真断言**（原先只是打印，看着像通过） |

Kotlin 侧代码（`kraos.platform.readU32` 等）已在裸机上真实执行并被反汇编验证
（`build/kernel.elf` 中存在 `kfun:kraos.platform#readU32(kotlin.Long){}kotlin.Long`）。

### 11.3 踩过的坑（都不该再踩第二次）

1. **K/N 2.1.255 没有可用的内联汇编。** 任何特权指令（`in/out/cli/hlt/sti/wrmsr/lgdt/lidt`、
   `syscall` 入口）都必须放在引导桩里、用 C ABI 暴露；`external` 声明不生成符号，
   cinterop 是唯一绑定路径。**GCC 编译的 `.c` 文件反而支持内联汇编** ——
   所以 C 侧能做汇编做不了的事。
2. **`@CName` 只在 K/N 运行时真正引用时才会导出。** glibc 内部符号必须是 C。
3. **gas 的 x86 后端不会在 `.rept` 里替换 `%`**，也**不允许对「节内符号」做移位**
   （`(vect >> 16) & 0xffff` 会报 *invalid operands*）。64 位 IDT 门描述符因此改为
   **在 C 里组装**，`.rept` 只用来展开无参数的宏。
4. **gas 的报错是中文的（`错误`）**，`make | grep error` 抓不到；必须 grep
   `'错误\|error:'` 并检查退出码，否则会复用上一次的陈旧 `kernel.elf`。
5. **SSE 第一条指令就 `#UD`**，根因不是 CPU 不支持，而是没开
   `CR4.OSXSAVE` → `xsetbv(XCR0=0x7)` → `CR4.OSFXSR` → `CR0.MP` → `fninit`。
6. **控制寄存器只能读进 64 位寄存器**：`movq %cr4,%rax`；`movl %cr4,%eax` 报 operand type mismatch。
7. **`syscall` 需要 `EFER.SCE`（bit 0）**，否则 `#UD`。MSR 号曾写错一格：
   `EFER=0xC0000080`、`STAR=0xC0000081`、`LSTAR=0xC0000082`、`SFMASK=0xC0000084`、
   `FS_BASE=0xC0000100`、`GS_BASE=0xC0000101`。
8. **`sysretq` 的 CS/SS 从 `STAR[63:48]` 推导**（CS=`+16`、SS=`+8`），对 3 条目的 GDT
   不可能同时满足。改用 **`iretq` + 显式 5 字帧**，且 RSP 必须**指向帧首**
   （`leaq 0x40(%rsp),%rsp`），不能从帧里加载。
9. **系统调用参数曾整体错位一格**（`rdi` 拿到 a0 而不是 nr），一次误传就把 56 MB 的
   `.rodata` 刷到了串口。定位这个 bug 的关键不是看串口，而是发现
   `kraos_serial_write` 的两条 `mov` 把长度参数顶掉了：`movq %rdi,%rsi` 之后
   `movq %rsi,%rcx` 拿到的是**缓冲区地址**而不是长度。
   **形参顺序与被调函数寄存器约定不一致时，先搬长度再搬缓冲区。**
10. **glibc ifunc**：`/usr/lib64/libc.a` 有 83 个 IFUNC 符号；lld 生成 `.iplt` 桩并**消费**掉
    IRELATIVE 重定位（`__rela_iplt_start == __rela_iplt_end`，`--emit-relocs` 无效）。
    解法是**自研 35 个实现 + `--wrap` 同时包住公开名与 glibc 的 `__` 隐藏别名**
    （`-linker-option=--wrap=` 传给 `ld.lld`）。现在 `.iplt` 已完全消失。
11. **`syscall_entry` 破坏了调用者的 `%rbx`。** 这是 P1 最后一个、也最隐蔽的阻塞点。
    入口写的是「`pushq %rbx` / `movq %rsp,%rbx` 记下栈指针 / 返回时再从帧里读回来」，
    等于把**被调用者保存寄存器** `rbx` 换成了原始栈地址。而 `syscall` 的 ABI
    并不承诺替调用者保住它。症状极具误导性：`initRuntime` 里 `new(296)` 明明返回了
    合法堆指针 `0x14016e0`，中间只隔着一次 `gettid()`（它 `jmp` 到 `syscall`），
    `rbx` 就变成了 `0x1255e40`；于是把栈上的垃圾当 `ThreadData` 写进
    `lazy[0]`，最后在 `AddRecord` 里 `#PF`。
    **教训：被调用者保存寄存器（`rbx`/`rbp`/`r12`–`r15`）在汇编桩里绝不能顺手当临时。**
    修法是只用 `r11`（`syscall` ABI 本就允许破坏）当临时，`rbx` 只在前两条指令里
    承载「原始 rsp - 8」并立即入帧。
12. **`futex` 桩不能对所有操作一律返回 `-EAGAIN`。** `FUTEX_WAKE` 拿到负数会把 glibc
    的条件变量逻辑彻底带偏，`pthread_cond_wait` 把 `EAGAIN` 当伪唤醒反复重试。
    现在按语义分开：`WAKE` 返 0（单线程没有等待者），`WAIT` 先比较 `*uaddr` 与
    `val`，不等才返 `EAGAIN`，相等则空转到超时后**明确报死锁并停机**（无声空转更糟）。
13. **`putChar` 直接发 `c.code and 0xFF` 会毁掉所有非 ASCII。** 串口是字节流，
    Kotlin 的 `Char` 是 UTF-16 码元。「内核启动」四个字的低字节连起来是
    `85 38 2f a8`——正是日志里出现的乱码。`putChar` 必须做 UTF-8 编码。
14. **Multiboot2 的 mmap 标签载荷开头还有 8 字节头**（`u32 entry_size` + `u32 entry_version`）。
    从偏移 0 就开始当描述符读，会把头当成第一条的 `base`/`len`，于是每个字段都错位，
    「可用内存」算出来是 0。步长也应该用标签自报的 `entry_size`，而不是写死 24。
15. **CPUID 叶 0xB 的枚举终止条件不能用 `ECX[15:0]`。** 规范说 level type 为 0 即无效，
    但 QEMU 对越界 subleaf 会把 `ECX` 原样填成 subleaf 序号（sub=2 回来 2、sub=3 回来 3……），
    永远非零。改用 `EBX == 0` 判定无效层级才稳。
    另外**总核数要取最后一个有效层级**的 `EBX[15:0]`，只看 subleaf 0（SMT 层）时
    无 SMT 的机器恒为 1。
16. **`CR4.OSXSAVE`(bit 18) 不能无条件写。** 这一位在没有 XSAVE 的处理器上是
    **保留位**，写它直接 `#GP`；而引导桩装 IDT 是在 SSE 初始化**之后**，
    于是 `#GP` 变成三重故障、机器静默复位。症状极像「内核死了」：串口只留下
    `[boot] 已进入 64 位长模式` 然后彻底没下文，连 `kraos_fault` 的异常打印都没有。
    QEMU 的**默认 CPU 模型 `qemu64` 就没有 XSAVE** —— 也就是说只要没显式加
    `-cpu max`，`make run` 必然停在这里。
    修法是整段 XSAVE 流程都先问 CPUID（`CPUID.1:ECX[26]=XSAVE`、`[27]=OSXSAVE`、
    `[28]=AVX`）再决定做不做；x86-64 架构上 SSE2 是强制基线且 `CR0.EM` 默认为 0，
    所以跳过 XSAVE 时 SSE 一样能用，只要补上 `CR4.MP`。
    **教训：装 IDT 之前的所有代码都不能依赖「出异常会被打印」这层保护，
    一个 `#GP` 就是一次静默重启。**
17. **`tools/ripdump.py` 里的两个坑**：QEMU 加 `-S` 时 CPU 停在复位态，
    `RIP=0xfff0` 还在 BIOS 区，这时读到的寄存器完全没有诊断价值 —— 必须先发 `c`
    放行、跑几秒、再发 `\x03` 打断。另外 RSP 的回复包**不保证是十六进制**
    （错误消息是原始字符串），不能假设「第一个 `$` 包就是 `g` 的回复」，
    要按「长度 272 且全是 hex 字符」筛。
18. **readelf 在中文 locale 下把标签翻成中文**，`grep -E 'Entry point'` 永远匹配不上，
    而 `grep` 失败又被 make 当成失败 —— 一度以为 ELF 坏了。所有管道都要 `LC_ALL=C`。

### 11.4 曾阻塞 P1 的问题及其定位过程（已解决）

> 这一节保留下来不是为了记录结论，而是为了记录**当时那套推断错在哪**，
> 以及在没有 gdb 的环境里怎么把问题钉死。结论是**四层**，从表到里依次剥开。

**当时的现象**：`#PF`，`CR2 = 0x1_0000_0001`，`RIP = ThreadLocalStorage::AddRecord+0x5d`，
`AddRecord` 的 `this`（来自 `rdi`）= `0x1255e40` —— 一个栈地址。

**当时的三条推断，全都判错了**，记在这里免得重犯：

| 当时的推断 | 实际情况 |
|---|---|
| FS 块布局猜错了，K/N 的 `__thread` 变量该放在 `fs:0` 所指对象的 `-0x48` | FS 布局**本来就是对的**。`fs:-0x48` 确实是 `ThreadRegistry::currentThreadDataNode_`，而且运行时确实被写成了合法堆指针 |
| `mov %fs:0x0,%rax` 出现 165 次，说明 K/N 把 `%fs:0` 当**指针**用 | 那些 `%fs:` 访问**全部来自 glibc**（`__pthread_mutex_lock_full`、`start_thread`、`pthread_setspecific`…），一个 K/N 指令都没有。`%fs:0` 是 TCB 自指针，K/N 的 `__thread` 变量在 TP 之下的**负偏移**处（`fs:-0x48`），因为链接期符号值是「TLS 段内偏移 0x48」而 `PT_TLS` 的 `memsz` 是 `0x90`：`0x48 - 0x90 = -0x48` |
| `rbx` 是在 `new()` 与 `initRuntime` 后续代码之间被**未检查的某个 callee** 破坏的 | 所有 callee 的 `push %rbx` / `pop %rbx` 都是配平的。破坏者是 `boot/entry.S` 里**我们自己写的** `syscall_entry`（见 §11.3 第 11 条） |

**没有 gdb 怎么定位**（本机没装 gdb，但 QEMU 的 `-gdb` 端口可以直接讲 RSP 协议）：

`tools/gdbwatch.py`、`tools/bpwalk.py`、`tools/bpmulti.py`、`tools/newtrace.py` 是为此写的
四个小工具，纯 socket 实现 gdb 远程协议（不需要 gdb 本体）。踩过的坑写在各文件头：

- QEMU 对每个请求先回一个裸 `+`，真正的回复是下一个包 —— 解析时必须把 `$` 之前的
  字节当垃圾丢掉；
- **别写 `data.startswith("O")` 判断控制台包**：成功回复就是 `"OK"`，以 `O` 开头，
  会被误判然后一直等下一个包；
- `g` 包里的寄存器是**目标机字节序**（x86_64 即小端）逐字节编码的，按大端整数读会得到
  `0x8b6e0101…` 这种明显错位的值；
- **TCG 模拟器不支持硬件观察点**（`Z2`/`Z3`/`Z4`），只能用软件断点 `Z0`；
- 每次给内核改代码，**所有地址都会平移**。任何一次反汇编结论都必须当场重取地址，
  上一轮的 `0x1016e8b` 很可能这一轮已经变成别的地方。

**关键一步**是用 `newtrace.py` 逐次跟踪 `operator new` 的入参与返回值，发现 12 次分配
**全部返回合法堆地址**（含出问题的那次 `new(296) = 0x14016e0`），再配合 `bpwalk.py`
逐步打印寄存器，才把范围缩到「`rbx` 在 `new()` 之后被改」，最后用 `bpmulti.py`
把断点下到窗口内每个函数入口，**点名了破坏者**。

### 11.5 验收现状

`make check` 是**真断言**，不是打印，全部通过：

| 断言 | 状态 | 说明 |
|---|---|---|
| multiboot2 头合法 | ✅ | `grub-file --is-x86-multiboot2` |
| 无 `PT_INTERP` | ✅ | 裸机没有解释器 |
| 入口链 `_start` → `call Konan_main` | ✅ | 见下方偏差说明 |
| 无 `INTERP` / `DYNAMIC` 段 | ✅ | |
| 无 `.iplt` ifunc 桩 | ✅ | `.rela.iplt` 残留 0 字节 |
| 串口打印 `KraOS boot ok` + 内存大小 + CPU 核数 | ✅ | `KraOS boot ok  mem=255 MiB  cpu=1` |
| Kotlin 代码在裸机上真实执行 | ✅ | 横幅本身由 Kotlin 打印；multiboot2 内存图逐条解析正确 |

> ⚠️ **与原始验收口径的一处偏差（需要知悉）**
> 原口径写的是「入口为 `Konan_main`」。实际上 ELF 入口**必须**是 `_start`：
> GRUB 在 32 位保护模式下跳转，映像的第一条指令必须是 32 位代码，
> 不可能是 64 位的 `Konan_main`（那会直接 `#GP`）。
> 因此断言改成了**等价且更强**的形式：ELF 入口 == `_start`，
> 且 `_start` 的长模式路径里确有 `call Konan_main`（用 `objdump` 结构性验证）。
> 「Kotlin 代码是顶层入口、底下没有 Linux 用户态启动链」这一意图仍然成立。

## 12. P2 进行中：物理页分配器 + 摘掉 glibc

> 状态：**前两项已完成并验证**，第三项（高半区）已探明全部障碍但尚未完成。

### 12.1 物理页帧分配器（已完成）

`kernel/src/kernel/PhysicalMemory.kt`，纯 Kotlin：读 `BootInfo.memoryRegions`，
位图逐页标记，`reserve` 先占住内核自己踩着的区间，再 `allocFrames` / `freeFrames`。
引导桩用 2 MiB 大页线性映射了 [0, 4 GiB)，所以 4 GiB 以下的物理地址可以直接当指针用。

真机自检（`make run` 输出）：

```text
[kraos] 物理页：84 / 255 MiB 已用（共 65504 帧）
[kraos] 页分配自检: 首帧=0x0000000005400000 64帧=0x0000000005401000 末帧=0x0000000005441000
[kraos] 页分配判定: 页对齐=✓ 互不相同=✓ 避开内核区=✓ 标记已用=✓ 释放归还=✓
```

**踩过的坑**：`findFreeFrom` 里用 `countTrailingZeroBits()` 找第一个 0 位，但
`countTrailingZeroBits(0) == 64` —— 整字全空时会指到本字之外，**每次分配都白白
跳过一整块（最多 64 帧）**。第一个可用帧恰好落在字边界（0x5400000 = 336×4096），
所以这个 bug 100% 触发。表现为「可用内存明明紧挨着，分配器却从中间跳过」。

### 12.2 摘掉 glibc 静态归档（已完成）

方法：把 `-lc -lpthread -ldl -lm` 从 `kraosLinkerExtraLibs.kraos_kernel` 里去掉，
让 ld 自己报缺什么 —— **得到的是精确清单，而不是靠猜**。一共 30 个符号，分两批：

| 批次 | 符号 |
|---|---|
| 第一批（20） | `abort` `clock_gettime` `__cxa_atexit` `dl_iterate_phdr` `fputc` `fputs` `free` `malloc` `mmap` `pthread_cond_broadcast` `pthread_cond_destroy` `pthread_mutex_lock` `pthread_mutex_unlock` `pthread_once` `snprintf` `vsnprintf` `__stack_chk_fail` `stderr` `strerror` `syscall` `write` |
| 第二批（10） | `calloc` `realloc` `_Exit` `dladdr` `open` `__fxstat` `pthread_detach` `pthread_getspecific` `pthread_setspecific` `sleep` |

全部实现在 `boot/libc_min.c`。结果：

- `make check` 全部通过；静态内核无 `PT_INTERP` / `DYNAMIC`，`nm` 里 glibc 内部符号
  （`__libc_start_main` / `__GI_` / `_nl_` / `_itoa`）**数量为 0**
- 当时摘库产物从 3,068,760 → 2,431,368 字节（**−21%**）；近期构建配置变化后以当前实测为准
- `KraOS boot ok mem=255 MiB cpu=1`

**C 兼容层裁剪**：`KraOSLinker` 链接行本来就无条件带 `--gc-sections`，但 GCC 编译
的 C 对象此前没有 `-ffunction-sections/-fdata-sections`，每个 C 文件的函数挤在一起，
无法按函数回收。Makefile 现已为 C 添加这两个选项。ELF 从 2,435,848 降到
2,428,400 字节（−7,448 字节）；最终 ELF 中 `libc_min.c` 导出的函数由 64 个降至 37 个，
`libc_shim.c` 由 19 个降至 11 个，`ifunc_shim.c` 仅保留 10 个包装函数。
本轮进一步把最终 ELF 中不可达的实现从源码删除：libc_min 27 个、libc_shim 7 个、
ifunc 51 个函数，并把 Makefile 的 ifunc `--wrap` 参数缩到这 10 个保留包装。
C 源码从初始 1,979 行降到 1,557 行（少 422 行）。源码删除不会进一步改变 ELF 大小，
因为这些函数体此前已被 section GC 丢弃。已用干净重建、`make check`（6 项全过）和
QEMU 4 vCPU `KraOS boot ok mem=255 MiB cpu=4` 验证。`nm -u` 仍显示 K/N 位码零引用的
`U __tls_get_addr` 及两个弱符号；没有未解决的可达重定位。

**为什么是 C 不是 Kotlin**：试过了。Kotlin 的 `@CName` 只有在**运行时位码本身**
引用了该符号时才生成对外可见的全局符号；仅被 Kotlin 代码调用是不够的，
`-opt` 会把它整个裁掉，链接时照样 undefined。另外 `pthread_mutex_t*`、`va_list`、
`FILE*` 都是 C ABI 边界，而 variadic function 在 Kotlin/Native 里根本没有对应物。
真正属于内核逻辑的部分（页分配器、内存图解析、控制台）仍在 Kotlin。

**「摘 libc 悬崖」**：补完第一批后链接器才继续往下解析，第二批才浮出来。
一层一层往下掉，直到清零。所以这个工作不能一次做完再验证，必须一轮一轮收敛。

**踩过的坑（值得单独记）**：`chunk_t` 头一开始是 5 个 8 字节字段 = **40 字节**，
不是 16 的倍数，于是 `malloc` 返回的载荷只有 8 字节对齐。而 K/N 运行时用
`movaps` 清零新对象，那条指令要求目的地址 16 字节对齐 → `#GP`，崩在
`initRuntime` 里一条看起来人畜无害的清零指令上。现在头部有显式填充字段，
外加一条**编译期断言** `typedef char kraos_hdr_aligned[(HDR % 16 == 0) ? 1 : -1];`
兜底 —— 这类 bug 的症状与病因隔着十万八千里，必须让编译器来说话。

### 12.3 高半区 0xffffffff80000000（已完成，已验机）

**结论先行**：内核主体现在真的跑在 `0xffffffff80000000`，6/6 引导矩阵 +
`make check` 5/5 全绿。`_start` 仍在低地址 `0x1001000`（GRUB 32 位入口只能跳这里），
长模式建立后一次 `movabsq $kraos_high_start,%r11; jmp *%r11` 跳进高半区，此后再不回落。

**先纠正一个错误认知**：计划里写「把 `kraosVirtualAddress.kraos_kernel` 改回
`0xffffffff80000000` 即可切高半区」，**这是错的**。看 `Linker.kt:479-491`：
`kraosVirtualAddress` 只在 `kraosLinkerScript` 为空时才被传给 `KraOSLinkerScript`。
我们配置了 `kraosLinkerScript.kraos_kernel = boot/linker.ld`，所以这个属性**没被使用**，
高半区完全由 `boot/linker.ld` 决定（属性仍设成 `0xffffffff80000000`，仅为一致性）。

**最终布局**（`boot/linker.ld` + `tools/checklayout.py` 可校验）：

- 6 个 `PT_LOAD`：低 `boot_text/boot_rodata/boot_data`（VMA==LMA，0x100x000 区）+
  高 `text/rodata/data`（VMA `0xffffffff8xxxxxxx`，LMA 0x54x000 区）。高段三段的
  `VMA-LMA` 差值必须是**同一个常量** `D`（引导桩靠它按 2 MiB 大页平铺映射）。
- 高段 LMA 由 `__kernel_lma`（2 MiB 对齐）起的 `__lma` 计数器经 `AT(__lma)` 显式给，
  每段放完用 `__lma = (__lma + SIZEOF(x) + 4095) & ~4095` 推进。

**踩过的四个障碍**（按遇到顺序，前三个是上一轮探明的）：

1. **同一 PT_LOAD 里不能混低地址与高地址段** —— lld 段大小溢出成
   `output file too large: 18446744071545620856`。解法：引导桩单独 `PHDRS`。
2. **孤儿段必须显式归位** —— `AT()` 之后 ld 不替我们管 `.gcc_except_table*` 等，
   每个输入段都要写出来。
3. **`R_X86_64_PLT32 out of range`** —— 低↔高差超 ±2 GB。解法：**所有可调用符号全放
   高段**；低→高只有一条 `movabsq`+间接跳；高段读低段数据一律 `movabsq`。
   **C 垫片用 `-mcmodel=kernel`**（不是 `-mcmodel=large`）：small model 的
   `R_X86_64_32`（零扩展）编不进高地址，kernel model 出 `R_X86_64_32S` 才行。
4. **高半区页表** —— `0xffffffff80000000` 规范化后 = `0xffff80000000` 的符号扩展，
   索引是 `PML4[511] → PDPT[510] → PD[0..n)`（**不是** PML4[256]！这是上次写错的）。
   `.bss.boot` 里新加 `hi_pdpt`/`hi_pd` 两张低页表，循环填 `__kernel_pde_count` 项。

**最后一个、最隐蔽的障碍（本轮才抓到）**：

5. **GRUB 保留区相交 → `overlap detected`。** QEMU 串口全空 + `you need to load the
   kernel first`，跟「内核没跑」一模一样。真相：GRUB 对每个 `PT_LOAD` 按
   `[p_paddr, p_paddr + p_memsz)` 向引导器申请保留区、按页取整，**相交就拒载**。
   两处成因，都已修：
   - **VMA 与 LMA 对齐量不一致**：`.eh_frame` 用 `ALIGN(8)`/`& ~7`，`.data` 用
     `ALIGN(4096)`，两条链累积出 0x1C6C 漂移，`.data` 的 LMA 掉进了 rodata 段的
     保留区里。修法：**高半区每一段的 VMA `ALIGN()` 与 `__lma` 的 `& ~` 都用 4096**，
     让差值 `D` 严格恒定（`checklayout.py` 第 2 项会查）。
   - **`.tbss` 挪到高半区末尾**：它归 `:rodata` 段，而 rodata 段的 `p_memsz` 由段内
     最高地址成员决定 —— 一个末尾的 `.tbss` 把整段跨度撑到几乎整个内核，保留区就
     盖住了 data 段。修法：**`.tbss` 紧跟 `.tdata`**（顺带把 `PT_TLS memsz` 压到
     0x38，远小于引导桩 64 KiB 的 %fs 块，双保险）。

**一个遗留的良性 `U`**：`nm -u build/kernel.elf` 会列 `U __tls_get_addr`。它在 K/N
位码符号表里登记过但**零引用**——无重定位、无调用点、无 GOT 项，lld 因此不报错。
两个 `w`（`__pthread_key_create`/`sched_yield`）是弱符号，解析到 NULL 安全。
别看到 `U` 就以为链接漏了。

### 12.4 P2 剩余欠账

- `libc_min.c` 的 `malloc` 仍从引导堆（`.bss` 里的 64 MiB 竞技场）取页，
  没有接到 12.1 的页分配器上。竞技场因此仍在，`-m 96M` 的下限也仍在。
- `futex` 仍是空转桩，多核调度（P3）前必须换成真实现
- IDT 仍只接异常，且 `iretq` 帧里强制 `IF=0`
- `__tls_get_addr` 目前零引用安全；若将来 K/N 真的发出 GD/LD 模型调用，要补真实现


— END —
