#!/usr/bin/env bash
# 从源码构建 KraOS 专用的 Kotlin/Native 编译器。
#
# 产出：.tooling/kotlin-src/kotlin-native/prepare/kotlin-native-compiler-embeddable/
#        build/libs/kotlin-native-compiler-embeddable.jar
#        用它替换官方发行版 konan/lib/ 下的同名 jar，即得到带 kraos_x64 的 konanc。
#
# 用法：KraOS/toolchain/konan-patch/build-kn.sh [额外 gradle 参数...]
set -euo pipefail

# kn-patch -> toolchain -> KraOS -> <workspace>
WORKSPACE="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$WORKSPACE/.tooling/kotlin-src"
TARGET_JAR="$SRC/kotlin-native/prepare/kotlin-native-compiler-embeddable"

# --- JDK ---
# K/N 的构建逻辑（gradle-settings-conventions 等）要求 JDK 8 toolchain。
# 本机只有 17/21，且 sudo 不可用，因此把 JDK 8 解包到 .tooling/jdk 并显式告知 Gradle。
[ -d "$WORKSPACE/.tooling/jdk" ] || { echo "error: 缺少 .tooling/jdk（JDK 8），先执行 setup-jdk8.sh" >&2; exit 1; }
export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-17-openjdk}"
export PATH="$JAVA_HOME/bin:$PATH"
export JDK_1_8="$WORKSPACE/.tooling/jdk"
export JDK_17_0="${JDK_17_0:-/usr/lib/jvm/java-17-openjdk}"
export JDK_18="$JDK_17_0"

# Gradle 以当前工作目录为构建根，必须先切进源码树再调用 wrapper
cd "$SRC"

# --- 本机网络环境的两个硬约束 ---
#
# 1) IPv6 不可用：curl -6 全部失败，且 maven.pkg.jetbrains.space / download.jetbrains.com
#    等域名会解析到 IPv6。必须强制 JVM 走 IPv4，否则解析或连接会随机失败。
# 2) 网络抖动频繁：github.com 的 git 协议被完全阻断，Maven 请求偶发断连。
#    因此把 Gradle 的重试次数与超时都放宽。
#
# 另外注意：:kotlin-native:* 这批工程在 settings.gradle 中由
# `buildProperties.isKotlinNativeEnabled` 守卫（读 Gradle 属性 kotlin.native.enabled），
# 不显式打开就报 "project 'kotlin-native' not found in root project 'kotlin'"。

exec "$SRC/gradlew" \
    :kotlin-native:prepare:kotlin-native-compiler-embeddable:assemble \
    -Pkotlin.native.enabled=true \
    -Dorg.gradle.jvmargs="-Xmx3g -XX:MaxMetaspaceSize=768m -Djava.net.preferIPv4Stack=true" \
    -Dorg.gradle.java.installations.paths="$WORKSPACE/.tooling/jdk,/usr/lib/jvm/java-17-openjdk,/usr/lib/jvm/java-21-openjdk" \
    -Dorg.gradle.internal.repository.max.retries=10 \
    -Dorg.gradle.internal.repository.initial.backoff=3000 \
    -Dorg.gradle.internal.http.connectionTimeout=180000 \
    -Dorg.gradle.internal.http.socketTimeout=180000 \
    --console=plain \
    "$@"

# --- 产物自检 ---
# ls -l "$TARGET_JAR/build/libs/"