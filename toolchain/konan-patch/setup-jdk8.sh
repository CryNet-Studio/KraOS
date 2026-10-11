#!/usr/bin/env bash
# 准备 K/N 构建所需的 JDK 8。
#
# 为什么需要：K/N 的构建逻辑工程（gradle-settings-conventions:* 等）用 JDK 8
# toolchain 编译。Gradle 默认会通过 foojay 自动下载 JDK 8，但下载会重定向到
# github.com/adoptium —— 而本机 github.com 的 git/CDN 链路被阻断，必然失败。
# 因此改为从 Amazon Corretto 直接取 tar 包，解包到 .tooling/jdk（无需 root）。
set -euo pipefail

WORKSPACE="$(cd "$(dirname "$0")/../../.." && pwd)"
DEST="$WORKSPACE/.tooling/jdk"
TGZ="$WORKSPACE/.tooling/jdk8.tgz"
URL="https://corretto.aws/downloads/latest/amazon-corretto-8-x64-linux-jdk.tar.gz"

if [ -x "$DEST/bin/javac" ]; then
    echo "[setup-jdk8] 已存在: $("$DEST/bin/java" -version 2>&1 | head -1)"
    exit 0
fi

echo "[setup-jdk8] 下载 $URL"
for i in 1 2 3; do
    curl -sL --fail -o "$TGZ" "$URL" || true
    if gzip -t "$TGZ" 2>/dev/null && [ "$(stat -c%s "$TGZ")" -gt 50000000 ]; then
        echo "[setup-jdk8] 完整性校验通过 ($(stat -c%s "$TGZ") 字节)"
        break
    fi
    echo "[setup-jdk8] 第 $i 次下载失败，重试"
    rm -f "$TGZ"; sleep 5
done
[ -s "$TGZ" ] || { echo "error: JDK 8 下载失败" >&2; exit 1; }

echo "[setup-jdk8] 解包到 $DEST"
mkdir -p "$DEST"
tar -xzf "$TGZ" -C "$DEST" --strip-components=1
"$DEST/bin/javac" -version
rm -f "$TGZ"
echo "[setup-jdk8] 完成"