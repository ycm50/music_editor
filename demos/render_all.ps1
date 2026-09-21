# 重新渲染 demos/ 下全部示例曲目 → demos/out/*.wav
#
# 用法:
#   pwsh -File demos\render_all.ps1 [save.exe 路径]

param([string]$Save = "")

$ErrorActionPreference = "Stop"
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not $Save) {
    foreach ($c in @("build\save\save.exe", "music_editor\build\save\save.exe")) {
        if (Test-Path $c) { $Save = $c; break }
    }
}
if (-not (Test-Path $Save)) {
    Write-Error "找不到 save 可执行文件, 请先构建或用参数指定路径, 例: pwsh -File demos\render_all.ps1 build\save\save.exe"
    exit 1
}

$out = Join-Path $dir "out"
New-Item -ItemType Directory -Force -Path $out | Out-Null

Write-Host "使用: $Save"
Write-Host "输出: $out"

# 单曲 (乐器写在文件内 @timbre)
Get-ChildItem (Join-Path $dir "*.rcp") | Where-Object { $_.BaseName -ne "02_timbre_compare" } | ForEach-Object {
    $dst = Join-Path $out ($_.BaseName + ".wav")
    & $Save $_.FullName --analyze -o $dst
}

# 音色对照: 同一段旋律 × 8 种乐器
Write-Host "--- 音色对照 ---"
foreach ($t in @("piano","violin","flute","guitar","harp","bells","music_box","organ")) {
    $dst = Join-Path $out "02_compare_$t.wav"
    & $Save (Join-Path $dir "02_timbre_compare.rcp") -T $t -o $dst
}

Write-Host "完成。可用 player 试听, 例如: player demos\out\02_compare_violin.wav"
