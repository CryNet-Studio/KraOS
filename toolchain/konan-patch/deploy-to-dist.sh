#!/usr/bin/env bash
# deploy-to-dist.sh — 将源码构建的 kraos / kraos_kernel 编译器 jar 部署进官方 K/N 发行版。
#
# 用法:
#   deploy-to-dist.sh <dist目录> [<compiler-embeddable.jar>]
#
#   <dist目录>  解压好的 kotlin-native-linux-x86_64-2.1.21 目录（须含 bin/konanc）
#   [jar]      默认为 .tooling/kotlin-src 下最近一次构建产物。
#
# 做的事（全部幂等，可重跑）:
#   1. 备份 konan/lib/kotlin-native-compiler-embeddable.jar → .orig，拷入新 jar
#   2. 备份 konan/konan.properties → .orig；清掉历史遗留的 kraos_x64 块；
#      若缺 kraos 块则从 kraos-targets.patch 提取并插到 "compilerVersion=" 注释前
#   3. bootstrap: klib/platform/{kraos,kraos_kernel}/stdlib（manifest native_targets 追加）
#   4. bootstrap: konan/targets/{kraos,kraos_kernel}/native/*.bc  ← 拷自 linux_x64
#   5. 验证: bin/konanc -list-targets 须同时出现 kraos 与 kraos_kernel
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCH="$SCRIPT_DIR/kraos-targets.patch"
WORKSPACE="$(cd "$SCRIPT_DIR/../../.." && pwd)"

# 两个目标：kraos_kernel 是 freestanding 内核，kraos 是 KraELF 用户态占位。
TARGETS=(kraos kraos_kernel)

DIST="${1:?用法: deploy-to-dist.sh <dist目录> [jar]}"
JAR="${2:-}"

if [[ -z "$JAR" ]]; then
    JAR="$(ls -t "$WORKSPACE/.tooling/kotlin-src/kotlin-native/prepare/kotlin-native-compiler-embeddable/build/libs/"kotlin-native-compiler-embeddable-*.jar 2>/dev/null | head -1)"
fi
[[ -f "$JAR" ]] || { echo "找不到编译器 jar: $JAR" >&2; exit 1; }
[[ -x "$DIST/bin/konanc" ]] || { echo "不是有效 K/N 发行版目录: $DIST" >&2; exit 1; }

echo "=== 1) 替换 embeddable jar ==="
cd "$DIST"
[[ -f konan/lib/kotlin-native-compiler-embeddable.jar.orig ]] \
    || cp -p konan/lib/kotlin-native-compiler-embeddable.jar konan/lib/kotlin-native-compiler-embeddable.jar.orig
cp "$JAR" konan/lib/kotlin-native-compiler-embeddable.jar
echo "  $(basename "$JAR") -> konan/lib/kotlin-native-compiler-embeddable.jar ($(du -h "$JAR" | cut -f1))"

echo "=== 2) 合并 konan.properties ==="
[[ -f konan/konan.properties.orig ]] \
    || cp -p konan/konan.properties konan/konan.properties.orig

# 历史遗留：早期版本合并过单目标 kraos_x64 块，必须先清掉，否则属性解析会留下
# 一堆无人引用的 linux_x64-kraos_x64 键。
if grep -q 'kraos_x64' konan/konan.properties; then
    python3 - "$PWD/konan/konan.properties" <<'PY'
import sys
kp = sys.argv[1]
lines = open(kp).read().split('\n')
out, i = [], 0
while i < len(lines):
    if lines[i].startswith('# KraOS 64-bit bare-metal target'):
        # 跳过整个旧块，直到撞上下一段顶层注释或空行后的新块
        while i < len(lines) and not lines[i].startswith('# The version of Kotlin/Native compiler'):
            i += 1
        continue
    out.append(lines[i]); i += 1
open(kp, 'w').write('\n'.join(out))
PY
    echo "  已移除遗留的 kraos_x64 块"
fi

if grep -q 'abiVersion.kraos_kernel' konan/konan.properties; then
    echo "  已含 kraos 双目标块，跳过"
else
    # 从补丁中提取 konan.properties 的新增行（保留空行），插到 compilerVersion 注释前
    awk '/^\+\+\+ b\/kotlin-native\/konan\/konan.properties/{f=1;next}
         /^--- /{f=0} f&&/^\+/{print substr($0,2)}' "$PATCH" > /tmp/kraos-block.$$
    python3 - "$PWD/konan/konan.properties" /tmp/kraos-block.$$ <<'PY'
import sys
kp, blk = sys.argv[1], sys.argv[2]
lines = open(kp).read().split('\n')
block = open(blk).read().split('\n')
i = next(i for i, l in enumerate(lines) if l.startswith('# The version of Kotlin/Native compiler'))
lines[i:i] = block + ['']
open(kp, 'w').write('\n'.join(lines))
PY
    rm -f /tmp/kraos-block.$$
    echo "  已插入 kraos 块"
fi
echo "  kraos 属性引用数: $(grep -c 'kraos' konan/konan.properties)"

echo "=== 3) bootstrap stdlib ==="
for t in "${TARGETS[@]}"; do
    if [[ -d klib/platform/$t/stdlib ]]; then
        echo "  klib/platform/$t/stdlib 已存在，跳过"
    else
        rm -rf "klib/platform/$t"          # 清掉可能残留的空壳
        mkdir -p "klib/platform/$t"
        cp -a klib/common/stdlib "klib/platform/$t/stdlib"
        sed -i "s/^native_targets=\(.*\)\$/native_targets=\1 $t/" \
            "klib/platform/$t/stdlib/default/manifest"
        echo "  已拷贝 klib/platform/$t/stdlib 并追加 native_targets"
    fi
done

echo "=== 4) bootstrap 运行时位码 ==="
for t in "${TARGETS[@]}"; do
    if [[ -d konan/targets/$t ]]; then
        echo "  konan/targets/$t 已存在，跳过"
    else
        rm -rf "konan/targets/$t"
        cp -a konan/targets/linux_x64 "konan/targets/$t"
        echo "  konan/targets/linux_x64 -> $t（$(ls konan/targets/$t/native/ | wc -l) 个 .bc）"
    fi
done
# 旧的单目标部署物，避免 konanc 目录里留下无人认领的目标
if [[ -d konan/targets/kraos_x64 || -d klib/platform/kraos_x64 ]]; then
    rm -rf konan/targets/kraos_x64 klib/platform/kraos_x64
    echo "  已清理遗留的 kraos_x64 部署物"
fi

echo "=== 5) 验证 konanc -list-targets ==="
missing=0
for t in "${TARGETS[@]}"; do
    if ./bin/konanc -list-targets | grep -qx "$t"; then
        echo "  ✓ $t 已注册"
    else
        echo "  ✗ 未找到 $t！" >&2
        missing=1
    fi
done
if [[ $missing -ne 0 ]]; then
    ./bin/konanc -list-targets
    exit 1
fi

echo "=== 完成: $DIST ==="
echo "试用内核:  $DIST/bin/konanc -target kraos_kernel hello.kt -o hello_kernel"
echo "试用用户态: $DIST/bin/konanc -target kraos hello.kt -o hello"