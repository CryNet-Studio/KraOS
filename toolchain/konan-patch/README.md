# konan-patch — KraOS 对 Kotlin/Native 的源码级改动

KraOS 通过**修改 Kotlin/Native 源码**新增两个自定义目标：freestanding 内核目标
**`kraos_kernel`** 与用户态目标 **`kraos`**，不从二进制 jar 做手术。
（旧 jar 方案已废弃，见 [`../kn-patch/DEPRECATED.md`](../kn-patch/DEPRECATED.md)。）

## 源码树

`.tooling/kotlin-src/` = Kotlin 单仓库 **v2.1.21** 全量源码，取自
[`.tooling/kotlin-v2.1.21.tar.gz`](../../.tooling/kotlin-v2.1.21.tar.gz)
（`https://codeload.github.com/JetBrains/kotlin/tar.gz/refs/tags/v2.1.21`）。

版本必须与所用 K/N 发行版一致——v2.1.21 的 `Linker.kt` 中 `class GccBasedLinker` 位于
L363、`fun linker()` 位于 L511（以上均指**纯净**上游树），与项目文档记录一致，可据此校验版本对错。

> ⚠️ `kotlin-src/` **不是 Git 仓库**，且从 tarball 重解会**覆盖所有改动**。
> 重解后必须重新应用本目录的补丁。

## 补丁

[`kraos-targets.patch`](kraos-targets.patch) —— 相对上游 v2.1.21 纯净树的改动，616 行、7 个文件：

| 文件 | 改动 |
| --- | --- |
| `native/utils/src/org/jetbrains/kotlin/konan/target/KonanTarget.kt` | 新增 `object KRAOS_X64`（目标名 `kraos`）与 `object KRAOS_KERNEL_X64`（目标名 `kraos_kernel`），注册进 `predefinedTargets` |
| `native/utils/src/org/jetbrains/kotlin/konan/target/HostManager.kt` | 两者加入 `commonTargets`，使 Linux 宿主可见 |
| `native/utils/src/org/jetbrains/kotlin/konan/target/Configurables.kt` | 新增接口 `KraOSConfigurables : GccConfigurables` |
| `native/utils/src/org/jetbrains/kotlin/konan/target/ConfigurablesImpl.kt` | 新增 `KraOSConfigurablesImpl`；`loadConfigurables()` 改为先按 `target.name.startsWith("kraos")` 优先分派 |
| `native/utils/src/org/jetbrains/kotlin/konan/target/Linker.kt` | 新增 `class KraOSLinker`；`linker()` 中新增 `is KraOSConfigurables` 分支 |
| `native/utils/src/org/jetbrains/kotlin/konan/target/KraOSLinkerScript.kt` | **新增文件**，内置链接脚本模板 |
| `kotlin-native/konan/konan.properties` | 新增 `*.kraos` 与 `*.kraos_kernel` 两套属性块 |

`KraOSConfigurables` 暴露的属性：`kraosLinkerScript` / `kraosEntryPoint` /
`kraosFreestanding` / `kraosLoadAddress` / `kraosVirtualAddress` /
`kraosLinkerExtraLibs` / `kraosExtraLinkerDirs` / `kraosKernelExcludedLibs` /
`kraosKraelfBase` / `kraosKraelfMagic`。

### `kraosExtraObjects` —— 往裸机链接里塞自研对象文件

```properties
kraosExtraObjects.kraos_kernel = /home/rj/KraOS/KraOS/build/entry.o,/home/rj/KraOS/KraOS/build/libc_shim.o,/home/rj/KraOS/KraOS/build/ifunc_shim.o
```

K/N 的链接器只认它自己那套位码/对象，**没有「外部加一个 `.o`」的入口**。
引导桩、内核 C 运行时、ifunc 替代实现这三块纯 C/汇编代码必须由 KraOS 自己编译再喂进去，
`kraosExtraObjects` 就是为此加的：逗号分隔的绝对路径，原样追加到链接命令行。

> ⚠️ **这三条对象里含 `entry.S`，它定义了 `_start`**，也就是 ELF 入口。
> 所以 `kraosEntryPoint.kraos_kernel` 保持 `Konan_main`（K/N 语义上的入口），
> 而真正的 **ELF 入口由链接脚本 / `-e` 指向 `_start`**。二者不冲突：
> `_start` 是 32 位 multiboot2 入口，跑在 GRUB 的保护模式下；
> `Konan_main` 是长模式里被 `call` 的第一个 Kotlin 符号。

> ⚠️ 改 `.tooling/kotlin-src/.../konan/konan.properties` 之后**必须同步**到
> `.tooling/kotlin-native-linux-x86_64-2.1.21/konan/konan.properties`
> （`deploy-to-dist.sh` 会做这件事）。

> ⚠️ `Linker.kt` 的 `linker()` 中，`is KraOSConfigurables` 分支**必须排在
> `is GccConfigurables` 之前** —— 前者是后者的子类型，顺序反了永远走不到裸机分支。

## 两个目标的分工

| | `kraos_kernel` | `kraos` |
| --- | --- | --- |
| 定位 | 裸机内核，**freestanding** | 用户态程序，KraELF 容器 |
| 状态 | ✅ 已实现并验证 | ⚠️ **占位，尚未实现** |
| `kraosFreestanding` | `true` | `false` |
| `kraosEntryPoint` | `Konan_main`（`launcher.bc` 里的 K/N 运行时 main） | `Konan_main` |
| 链接分支 | `KraOSLinker` 裸机分支 | 退化为 hosted gnu 形状 |
| 保留属性 | — | `kraosKraelfBase=0x400000`、`kraosKraelfMagic=0x4b52414f53`（预留，容器格式未设计） |

`kraos_kernel` **完全不走 gnu 启动约定**：不引入 `crt1.o`/`crti.o`/`crtbegin.o`/
`crtend.o`/`crtn.o`，不加 `-dynamic-linker`/`--sysroot`/`-export-dynamic`/`--eh-frame-hdr`，
段布局与入口由 `-T <链接脚本>` + `--entry=Konan_main` 决定，链接行形如：

```text
ld.lld --entry=Konan_main -T <链接脚本> --no-dynamic-linker -static \
       -z noexecstack --gc-sections --build-id=sha1 -o <产物> ...
```

`kraosLinkerScript` 留空时，编译器把 `KraOSLinkerScript` 的内置脚本现写到临时文件
（`/tmp/kraos-linker-scripts/kraos-kernel.ld`）；配成绝对路径即用用户自定义脚本接管布局。

## 重新应用

> 本补丁**已验证可完整复现**当前 `.tooling/kotlin-src/` 的全部改动：
> 解开纯净 v2.1.21 树 → `patch -p1` → 与现树逐文件 `diff`，7/7 一致。
> 校验方式见 `HANDOFF.md` §11 末尾的「补丁可复现」步骤。

```bash
cd /home/rj/KraOS/.tooling/kotlin-src
patch -p1 --dry-run < /home/rj/KraOS/KraOS/toolchain/konan-patch/kraos-targets.patch   # 先试
patch -p1            < /home/rj/KraOS/KraOS/toolchain/konan-patch/kraos-targets.patch
```

校验：

```bash
cd /home/rj/KraOS/.tooling/kotlin-src
T=native/utils/src/org/jetbrains/kotlin/konan/target
grep -c 'KRAOS_X64\|KRAOS_KERNEL_X64' $T/KonanTarget.kt $T/HostManager.kt
grep -c 'KraOSLinker\|KraOSConfigurables' $T/Linker.kt
grep -c kraos kotlin-native/konan/konan.properties
ls $T/KraOSLinkerScript.kt
# 期望：KonanTarget=5, HostManager=2, Linker=7, konan.properties=74
```

## 构建

```bash
/home/rj/KraOS/KraOS/toolchain/konan-patch/build-kn.sh
```

脚本已封装三个坑：JDK 8 toolchain（`.tooling/jdk`）、`kotlin.native.enabled` 守卫、
强制 IPv4 + 抗抖动重试。产物：
`kotlin-native/prepare/kotlin-native-compiler-embeddable/build/libs/kotlin-native-compiler-embeddable-2.1.255-SNAPSHOT.jar`。

整轮构建约 **5-6 分钟**，产物 jar 约 **279MB**。`BUILD SUCCESSFUL` 后胖 jar 中
`KonanTarget`/`HostManager` 均能引用到两个 KRAOS 目标。

## 部署到发行版（端到端已验证）

官方发行版下载 **不走 github**（被墙），走 JetBrains 自家 CDN（本机可达，
重定向到 `download-cdn.jetbrains.com.cn`）：

```bash
curl -L -o kotlin-native-linux-x86_64-2.1.21.tar.gz \
  "https://download.jetbrains.com/kotlin/native/builds/releases/2.1.21/linux-x86_64/kotlin-native-linux-x86_64-2.1.21.tar.gz"
# 174MB；URL 规则见 KGP 的 NativeCompilerDownloader.BASE_DOWNLOAD_URL
```

部署（幂等，已在纯净 dist 上验证）：

```bash
/home/rj/KraOS/KraOS/toolchain/konan-patch/deploy-to-dist.sh \
    /home/rj/KraOS/.tooling/kotlin-native-linux-x86_64-2.1.21
```

脚本完成 5 件事：备份并替换 `konan/lib/kotlin-native-compiler-embeddable.jar`；
把补丁里的 `*.kraos` / `*.kraos_kernel` 块并入 `konan/konan.properties`；对两个目标分别
bootstrap `klib/platform/<target>/stdlib`（manifest `native_targets` 追加目标名）；
`konan/targets/<target>/` ← 拷 linux_x64 运行时位码（30 个 .bc）；
`bin/konanc -list-targets` 验证两者都在列。

脚本已改成 `for t in (kraos kraos_kernel)` 的双目标循环，**并会清理早期单目标
`kraos_x64` 的遗留部署物**（`konan/targets/kraos_x64`、`klib/platform/kraos_x64`）
与 properties 旧块，否则属性解析会留下一堆无人引用的 `linux_x64-kraos_x64` 键。

**`kraos_kernel` 端到端已验证**：

```bash
konanc -target kraos_kernel hello.kt -o hello_kernel     # → hello_kernel.kexe
readelf -h hello_kernel.kexe     # Type: EXEC (Executable file, not PIE)，Entry 0x128180
readelf -l hello_kernel.kexe     # Program Headers 里 PT_INTERP 数量为 0
```

无 `.interp`/`.dynamic`/`.plt`/`.got` 段；`.text` 落在 `0x100000`，`.bss` 为 NOBITS；
符号齐全：`__kernel_start`、`__bss_start`、`__bss_end`、`__kernel_end`、`Konan_main`。
`-p library` 也能正常产出 klib。

**`kraos` 目前只能产出普通 ELF**（占位）：`konanc -target kraos hello.kt -o hello_user`
→ `hello_user.kexe`，Type EXEC、Entry `0x428180`、无 PT_INTERP，链接成功时打印警告：

```text
warning: target kraos produces a plain ELF for now; KraELF container generation is not implemented yet.
```

## konan.properties 坑（本轮踩实）

`hostTargetString(key)` 的键查找顺序是 `key.<host>-<target>` → **裸 key**，
**不会**回退 `key.<target>`（`compiler/util-io/.../properties/Properties.kt:51`）。
因此 toolchain 相关键必须写成 `*.linux_x64-kraos` / `*.linux_x64-kraos_kernel`
（host-target 顺序，**不是** `kraos-linux_x64`）：`llvmHome`、`targetToolchain`、`linker`、
`linkerHostSpecificFlags`、`dependencies`。只写 `*.kraos` 会在
`-produce program` 时于 `ClangArgs.<init>` → `absoluteTargetToolchain` 抛 NPE。

另一个坑：freestanding 下**不能**复用 `linkerGccFlags` —— 那是个带 `--as-needed` 与动态库
语义的全局默认键，会把内核产物拖回动态链接。所以另起了 `kraosLinkerExtraLibs`
（`-lstdc++ -lpthread -ldl -lc -lm`）逐个点名静态库，并刻意不加 `-Bdynamic`
（`-Bdynamic` 会让 `ld.lld` 去挑 `.so`）。

## 设计取舍

**两个目标都复用 `Family.LINUX`**，没有新增 `Family.KRAOS`。原因是 K/N 的工具链查找、
sysroot 解析、库选择等大量逻辑按 Family 分派，新增 Family 会牵连编译器里所有
`when (family)` 穷尽分支，代价远大于收益。KraOS 与 Linux 的实际差异全部下沉到
konan.properties 条目与 `KraOSLinker` 实现；配置侧的差异则由
`KraOSConfigurables`（继承 `GccConfigurables` 以复用工具链解析）承载，
`loadConfigurables()` 按 `target.name.startsWith("kraos")` 优先分派给
`KraOSConfigurablesImpl`，从而避开 Family 分派。

## 当前已知限制

引导期 **VMA 暂设 `0x0`、LMA `0x100000`**（实际链接在 `0x100000`）。因为借用的 glibc
静态归档用 `R_X86_64_32` 绝对重定位，放不下 `0xffffffff80000000` 高半区，会报
`relocation R_X86_64_32 out of range`。自研运行时位码落地、静态 libc 被摘掉之后，把
`kraosVirtualAddress.kraos_kernel` 改回 `0xffffffff80000000` 即切高半区。

## 后续待办（对应 HANDOFF.md）

- [ ] 自研运行时位码：替换 `konan/targets/kraos_kernel/native/*.bc` 的 linux_x64 占位
- [ ] stdlib 平台 actual：替换借用自 common stdlib 的 klib
- [ ] 自研位码落地后：清空 `kraosLinkerExtraLibs.kraos_kernel`、把 VMA 切回高半区
- [ ] 设计并实现 KraELF 容器格式与加载器，让 `kraos` 目标真正产出 KraELF