#!/usr/bin/env bash
#
# 本机没有 Maven 时的应急重建脚本。
#
# 为什么需要它:jar-obfuscator fork 改了几个源文件,但要重新产出 fat jar 得靠 Maven,
# 而开发机上没装(见 README 里的 TODO)。这个脚本用 javac 对着**已有的 fat jar**
# 编译改过的源文件,再把新的 class 回填进去。
#
# 装好 Maven 之后应该用 `mvn package` 取代它 —— 这里只是权宜之计。
#
# 用法:
#   ./rebuild-fork.sh                     # 重建下面 FILES 里列出的源文件
#   ./rebuild-fork.sh <a.java> [b.java]   # 只重建指定文件(路径相对仓库根)
#
# ⚠ 两个坑:
#   1) /mnt/A 是挂载盘,刚写的文件对新启的进程有可见性延迟。
#   2) 正因为如此,`jar uf <jar> -C <dir> a.class b.class` 这种**多文件**形式
#      会打印 "没有这个文件或目录" 却仍然返回 0 —— 静默什么都不做。
#      所以这里每个 class 都单独调用一次 jar。
#
set -u

cd "$(dirname "$0")" || exit 1

FAT="target/jar-obfuscator-2.0.1-jar-with-dependencies.jar"
THIN="target/jar-obfuscator-2.0.1.jar"
SRC_ROOT="src/main/java"

# fork 改动过的文件清单。加了新的改动就补在这里。
FILES="
$SRC_ROOT/me/n1ar4/jar/obfuscator/loader/CustomClassLoader.java
$SRC_ROOT/me/n1ar4/jar/obfuscator/loader/CustomClassWriter.java
$SRC_ROOT/me/n1ar4/jar/obfuscator/transform/XORTransformer.java
$SRC_ROOT/me/n1ar4/jar/obfuscator/transform/StringTransformer.java
$SRC_ROOT/me/n1ar4/jar/obfuscator/transform/JunkCodeTransformer.java
"

if [ $# -gt 0 ]; then
    FILES="$*"
fi

if [ ! -f "$FAT" ]; then
    echo "找不到 $FAT" >&2
    echo "它必须已经存在(由 Maven 构建过一次),本脚本只做增量重编译。" >&2
    exit 1
fi

echo "=== 编译(pom 的目标是 Java 8,major 52)==="
# shellcheck disable=SC2086
javac --release 8 -encoding UTF-8 -proc:none \
      -cp "$FAT:target/classes" -d target/classes $FILES
rc=$?
if [ $rc -ne 0 ]; then
    echo "javac 失败(退出码 $rc),jar 未改动" >&2
    exit $rc
fi
echo "  javac 退出码 = 0"

echo
echo "=== 回填(每个 class 单独一次 jar 调用)==="
fail=0
for src in $FILES; do
    case "$src" in
        "$SRC_ROOT"/*) rel="${src#$SRC_ROOT/}" ;;
        *) echo "  跳过(不在 $SRC_ROOT 下): $src" >&2; continue ;;
    esac
    cls="${rel%.java}.class"

    if [ ! -f "target/classes/$cls" ]; then
        echo "  ✗ 没有产出 target/classes/$cls" >&2
        fail=1
        continue
    fi
    for jar in "$FAT" "$THIN"; do
        ( cd target/classes && jar uf "../../$jar" "$cls" )
    done
    echo "  ✓ $cls"
done

echo
echo "=== 核验:jar 里的 class 与 target/classes 大小一致 ==="
bad=0
for src in $FILES; do
    case "$src" in
        "$SRC_ROOT"/*) rel="${src#$SRC_ROOT/}" ;;
        *) continue ;;
    esac
    cls="${rel%.java}.class"
    a=$(stat -c%s "target/classes/$cls" 2>/dev/null || echo 0)
    b=$(unzip -l "$FAT" "$cls" 2>/dev/null | awk '/\.class$/ {print $1}' | head -1)
    if [ "$a" = "$b" ]; then
        echo "  ✓ $cls ($a 字节)"
    else
        echo "  ✗ $cls 目录里 $a / jar 里 ${b:-缺失}" >&2
        bad=1
    fi
done

if [ $fail -ne 0 ] || [ $bad -ne 0 ]; then
    echo
    echo "有文件没回填成功。挂载盘偶尔抽风,再跑一次本脚本通常就好。" >&2
    exit 1
fi

echo
echo "完成。"
