#!/usr/bin/env bash
# 重新渲染 demos/ 下全部示例曲目 → demos/out/*.wav
#
# 用法:
#   bash demos/render_all.sh [save可执行文件路径]
# 默认使用 build/save/save.exe (Windows) 或 build/save/save (Linux)

set -euo pipefail

SAVE="${1:-}"
if [ -z "$SAVE" ]; then
    for c in build/save/save.exe build/save/save music_editor/build/save/save.exe music_editor/build/save/save; do
        [ -x "$c" ] && SAVE="$c" && break
    done
fi
if [ -z "$SAVE" ] || [ ! -x "$SAVE" ]; then
    echo "找不到 save 可执行文件, 请先构建或用参数指定路径" >&2
    exit 1
fi

DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="$DIR/out"
mkdir -p "$OUT"

echo "使用: $SAVE"
echo "输出: $OUT"

# 单曲 (乐器写在文件内 @timbre)
for f in "$DIR"/*.rcp; do
    base="$(basename "$f" .rcp)"
    [ "$base" = "02_timbre_compare" ] && continue
    "$SAVE" "$f" --analyze -o "$OUT/$base.wav" | sed 's/^/  /'
done

# 音色对照: 同一段旋律 × 8 种乐器
echo "--- 音色对照 ---"
for t in piano violin flute guitar harp bells music_box organ; do
    "$SAVE" "$DIR/02_timbre_compare.rcp" -T "$t" -o "$OUT/02_compare_$t.wav" | sed 's/^/  /'
done

echo "完成。可用 player 试听, 例如: player demos/out/02_compare_violin.wav"
