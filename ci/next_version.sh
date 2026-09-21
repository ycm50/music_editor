#!/usr/bin/env bash
# 计算下一个版本号: 读仓库最新 tag (形如 1.2), 次版本 +1, 满 9 进位
#   无 tag      -> 1.1
#   1.9         -> 2.1
# 输出到 GITHUB_OUTPUT: version / prev
# 也可本地试跑: bash ci/next_version.sh
set -euo pipefail

latest=$(git tag --list | grep -E '^[0-9]+\.[0-9]+$' | sort -V | tail -1 || true)

if [ -z "$latest" ]; then
    prev="无"
    version="1.1"
else
    major=${latest%.*}
    minor=${latest#*.}
    if [ "$minor" -ge 9 ]; then
        major=$((major + 1))
        minor=1
    else
        minor=$((minor + 1))
    fi
    prev="$latest"
    version="$major.$minor"
fi

if [ -n "${GITHUB_OUTPUT:-}" ]; then
    {
        echo "prev=$prev"
        echo "version=$version"
    } >> "$GITHUB_OUTPUT"
fi

echo "下一版本 = $version (上一版本 = $prev)"
