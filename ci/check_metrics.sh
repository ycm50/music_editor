#!/usr/bin/env bash
# 校验合成结果与基准文件逐字节一致 (跨平台/跨编译器一致性)
#
# 用法: ci/check_metrics.sh <save 可执行文件> <基准文件>
#
# 基准文件由 `save <demo> --metrics` 生成, 第 1 列是乐谱文件名 (仅基名), 其余是字段。
# 只比较与平台无关的确定性数值。
#
# 容错: 自动去掉 UTF-8 BOM 与 CRLF —— 基准文件可能在 Windows 上编辑/生成,
# 否则 Windows 与 Linux 的基准看起来"不一致"而实际数值相同。
set -euo pipefail

SAVE="${1:?需要 save 可执行文件路径}"
GOLDEN="${2:?需要基准文件路径}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

if [ ! -f "$GOLDEN" ]; then
    echo "基准文件不存在: $GOLDEN" >&2
    exit 2
fi

SAVE="$(cd "$(dirname "$SAVE")" && pwd)/$(basename "$SAVE")"

# 归一化基准: 去 BOM / 去 CR / 去尾部空行
normalize() {
    sed -e '1s/^\xEF\xBB\xBF//' -e 's/\r$//' -e '/^[[:space:]]*$/d' "$1"
}

expected="$(mktemp)"
actual="$(mktemp)"
trap 'rm -f "$expected" "$actual"' EXIT
normalize "$GOLDEN" > "$expected"

missing=0
while read -r demo _rest; do
    [ -z "$demo" ] && continue
    score="demos/$demo"
    [ -f "$score" ] || score="music_editor/$demo"
    if [ ! -f "$score" ]; then
        echo "基准里引用的乐谱不存在: $demo" >&2
        missing=1
        continue
    fi
    "$SAVE" "$score" --metrics \
        | sed 's|^METRICS ||; s|\\|/|g; s|[^ ]*/||; s/\r$//' \
        >> "$actual"
done < "$expected"

[ "$missing" -eq 0 ] || exit 2

if diff -u "$expected" "$actual"; then
    echo "合成一致性: 与基准完全一致 ✅ ($(wc -l < "$expected") 条)"
else
    echo "" >&2
    echo "合成一致性: 与基准不一致 ❌" >&2
    echo "若确实改了合成算法/乐器参数, 请同步更新基准:" >&2
    echo "  for d in demos/*.rcp; do \"\$SAVE\" \"\$d\" --metrics | sed 's|^METRICS ||; s|\\\\|/|g; s|[^ ]*/||'; done > ci/metrics_golden.txt" >&2
    exit 1
fi
