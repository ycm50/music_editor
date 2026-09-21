# RCP 简谱编辑器

基于简谱（数字谱）的音乐编辑与播放工具，单仓库同时提供 **桌面端**（C++20 + Qt6）与 **安卓端**（Kotlin + C++/JNI）。两端共用同一套纯 C++ 核心库完成**解析、物理合成与 WAV 编码**，保证行为一致。

## 核心特点

- **物理乐器模型**：分音频率含劲度非谐性 `f_n = n·f₁·√(1+B·n²)`，分音包络按**绝对时间**指数衰减 `A_n(t)=A_n(0)·(sus+(1-sus)·e^{-t/τ_n})`，与音符长短解耦——弹 1 拍与弹 4 拍音色一致
- **8 种内置乐器**：piano / violin / flute / guitar / harp / bells / music_box / organ，也可用 `@acoustic` 自定义物理参数
- **多声部与和弦**：`@voice` 独立时间轴纵向叠加，`[1.04+3.04+5.04]` 纵向和弦，`_` 真正连音（时值相加、只起音一次）
- **乐理完整度**：升降号 `#4`/`b7`、附点 `2.`、三连音 `4t`、力度 `!pp..!ff`、拍号 `@meter` 校验
- **可验证**：`--analyze` 输出音域、频谱重心、衰减时间常数、直流、混叠、小节对齐等客观指标

## 功能

- **RCP 乐谱**：自有文本谱格式（头部 + `@` 元数据 + 音符），带注释与多声部
- **导出 WAV**：立体声（Schroeder 混响）/ 单声道，16-bit PCM
- **实时播放**：桌面端 Qt Multimedia（自动选择设备支持的音频格式），安卓端 AudioTrack
- **AI 生成**（桌面端 + 安卓端）：OpenAI 兼容接口，一键生成乐谱、乐器与力度，流式输出；系统提示词与可自检示例的唯一来源在 `music_editor/core/ai_prompt.cpp`，两端共用同一份文本
- **自检工具**：`tools/check_prompt`（断言提示词示例合法）+ `tools/test_syntax`（记法单元测试）
- **一键构建**：GitHub Actions 手动触发（或打 tag），一次产出 Windows (x64/arm64) zip、Linux (x64/arm64) tar.gz 与安卓 APK

## 项目结构

```
.
├── music_editor/                   桌面端 (C++20 + CMake + Qt6)
│   ├── core/                       纯 C++ 共享库 (无 Qt 依赖, 桌面/安卓复用)
│   │   ├── note_parser             RCP 解析 (音符/元数据/物理参数/乐理校验)
│   │   ├── tone_gen                物理合成 + 乐器库 + 混响 + 体检指标
│   │   ├── ai_prompt               AI 系统提示词 + 可自检示例 (两端唯一来源)
│   │   └── wav_writer              16-bit PCM WAV 编码/写入 (含立体声)
│   ├── player/                     命令行播放器 (Qt6::Multimedia)
│   ├── save/                       RCP→WAV 转换 + --analyze 体检 (纯 C++, 无 Qt)
│   └── ui/                         GUI 编辑器 (Qt6::Widgets, GUI 子系统无终端窗口)
├── tools/                          自检工具 (check_prompt / test_syntax)
├── demos/                          示例曲目 (.rcp 源码 + out/*.wav 试听)
├── android/                        安卓端 (Kotlin + NDK/JNI)
│   └── app/src/main/cpp/           JNI 桥接, 直接引用 music_editor/core 源码
└── .github/workflows/build.yml     多平台构建矩阵 (本仓库唯一的 workflow)
```

## 快速开始（桌面端）

### 依赖

- 编译器：GCC 13+ / MinGW 15+ / MSVC 2022+（C++20）
- CMake ≥ 3.16，构建工具 Ninja 或 MinGW Makefiles
- Qt 6.x：`player` 需 `Qt6::Multimedia`，`ui` 需 `Qt6::Widgets`；`save` 与 `core` 不依赖 Qt

MSYS2 (UCRT64) 一键安装：

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-qt6-base \
    mingw-w64-ucrt-x86_64-qt6-multimedia \
    mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
```

### 构建

```bash
cmake -S music_editor -B music_editor/build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=A:/msys64/ucrt64
cmake --build music_editor/build --parallel
```

> Windows 注意：**构建路径不要含中文**，否则 Qt 的 `moc` 无法创建输出文件。若源码目录含中文，可先建一个 ASCII 目录联接再构建：
> `mklink /J A:\tmp\me-boot "A:\你的路径\music_editor"`，然后对 `A:\tmp\me-boot\music_editor` 执行 cmake。

### 运行

`player` / `ui` 需要 Qt DLL 在 PATH 中（MSYS2 为 `A:\msys64\ucrt64\bin`）。发布包已内置全部 DLL，直接运行即可。

```bash
# 命令行播放 (乐器名可用 --list 查看)
build/player/player.exe demos/01_piano_nocturne.rcp --instrument piano
# 导出 WAV + 体检报告
build/save/save.exe demos/01_piano_nocturne.rcp --analyze -o out.wav
# 同谱 A/B 对比音色 (强制覆盖文件内 @timbre)
build/save/save.exe demos/02_timbre_compare.rcp -T violin -o cmp_violin.wav
# 查看可用乐器及其物理参数
build/save/save.exe --list
# 图形界面 (无终端窗口; 自动查找同目录或 ../ 下的 player/save)
build/ui/ui.exe
```

`ui` 打包后为单目录平铺（`ui.exe`、`player.exe`、`save.exe` 及 Qt DLL 同目录）；开发目录布局（`build/ui`、`build/player`、`build/save` 分目录）同样支持。

> **启动脚本闪退（找不到 Qt DLL）**：`run_ui.bat` / `run_player.bat` 会自动探测 Qt6 的 `bin` 目录并加入 `PATH`（探测 `qmake6.exe`，再依次尝试 `A:\msys64\ucrt64\bin`、`A:\msys2\ucrt64\bin`、`A:\msys64\mingw64\bin`、`C:\msys64\...`、`A:\Qt\6.*\mingw_64\bin` 等）。若你的 Qt 装在别处，直接编辑脚本首部设为：
> ```bat
> set "QTBIN=D:\你的路径\Qt\6.x.x\mingw_64\bin"
> ```
> 脚本会打印 `[run_ui] Qt6 found at: ...`，据此可确认探测结果。纯命令行方式也可手动设置：`set PATH=A:\msys64\ucrt64\bin;%CD%\build\ui;%PATH%`。


> `ui` 与手机端一致：单个编辑框直接编辑完整 RCP 内容（头部/`@` 元数据/音符），参数从编辑框内容解析；内置 AI 生成谱（OpenAI 兼容接口，流式输出）、复制提示词按钮，在"设置 (AI)"中配置 Base URL / API Key / 模型。编辑内容与 AI 设置持久化到 `ui.exe` 同目录的 `config.json`，下次启动自动恢复。生成后会跑一遍 `--analyze` 体检，把乐理/格式问题显示在状态栏。

## 示例曲目（demos/）

源码 `.rcp` 与渲染好的 `demos/out/*.wav` 一并提供，可直接试听对比：

| 文件 | 乐器 | 内容 | 时长 |
|---|---|---|---|
| `01_piano_nocturne.rcp` | piano | C 大调夜曲，双声部（右手旋律 + 左手和弦），连音与力度 | 119 s |
| `02_timbre_compare.rcp` | ×8 | **同一段旋律**分别用 8 种乐器演奏（`out/02_compare_*.wav`） | 63 s |
| `03_musicbox_jingle_bells.rcp` | music_box | 圣诞 · Jingle Bells，`!f/!mf/!p` 力度对比 | 57 s |
| `04_organ_minuet.rcp` | organ | 巴洛克 · G 大调小步舞曲风，3/4 拍 | 51 s |
| `05_harp_pentatonic.rcp` | harp | 中国风 · 五声音阶羽调式，琶音和弦 | 110 s |
| `06_piano_chords.rcp` | piano | 练习曲 · 和声型（C–Am–F–G–C），检验和弦纵向叠加 | 110 s |

重新生成：

```bash
bash demos/render_all.sh build/save/save.exe            # MSYS2 / Linux
pwsh -File demos/render_all.ps1 build\save\save.exe     # Windows
```

## 安卓端

```bash
cd android
./gradlew :app:assembleRelease          # 或 assembleDebug
```

- C++ 核心经 JNI 暴露 `getInstruments` / `getTimbres` / `getTimbreHarmonics` / `renderPcm` / `renderWav` / `analyze`（立体声）
- 解析与合成**全部**在 C++ 侧完成（`parse_rcp` + `render_document`），Kotlin 只负责传文本与播放，与桌面端严格一致
- 编辑、播放（AudioTrack 立体声）、导出 WAV 到下载目录、导入 RCP
- AI 生成谱与乐器：设置页配置 OpenAI 兼容 `base_url` / `api_key` / `model`；菜单可一键复制系统提示词

## CI 构建

| Workflow | 触发 | 做什么 |
|---|---|---|
| `build.yml` | 手动 (`workflow_dispatch`) / 打 tag / 被其他 workflow 复用 | 多平台构建矩阵（下表），产物统一命名 |

> 仓库里只保留这一个 workflow。原来负责「每次 push 快速校验」的 `ci.yml` 与「算版本 + 发 Release」的
> `release.yml` 已移除；矩阵里的 Linux 两个 job 本来就会跑 `ctest` 与合成一致性比对，所以那几道闸门并没有丢。

### 构建矩阵

| 目标 | runner | 工具链 | 产物 |
|---|---|---|---|
| Windows x86_64 | `windows-latest` | MinGW 13.1 + Qt6 `win64_mingw` + `windeployqt` | `music-editor-windows-x86_64-<v>.zip` |
| Windows aarch64 | `windows-latest` | MSVC arm64 交叉编译 + Qt6 `win64_msvc2022_arm64_cross_compiled`（+ 同版本 x64 host Qt 作 `QT_HOST_PATH`） | `music-editor-windows-aarch64-<v>.zip` |
| Linux x86_64 | `ubuntu-24.04` | apt `qt6-base-dev` + `qt6-multimedia-dev` | `music-editor-linux-x86_64-<v>.tar.gz` |
| Linux aarch64 | `ubuntu-24.04-arm`（原生 arm64 runner） | 同上 | `music-editor-linux-aarch64-<v>.tar.gz` |
| Android arm64-v8a / x86_64 | `ubuntu-24.04` | JDK 17 + Android SDK（AGP 自动下载 NDK/CMake） | `music-editor-android-<v>.apk` |

> **关于 Windows aarch64**：Qt 官方没有 Windows ARM64 的 MinGW/llvm-mingw 二进制包（`win64_llvm_mingw` 是 **x86_64** 目标），
> 只有 MSVC 构建，且 6.8.x 在 x64 主机上能装的是交叉编译包 `win64_msvc2022_arm64_cross_compiled`
> （原生 `win64_msvc2022_arm64` 需要 ARM64 主机 runner，如 `windows-11-arm`）。
> 交叉包只含 ARM64 二进制，必须再提供同版本 x64 host Qt 作为 `QT_HOST_PATH`；产物是 ARM64 exe，
> 在 x64 runner 上无法执行，因此该 job 不跑 `ctest`，改为校验产物 PE 头（`e_machine == 0xAA64`），
> 且 zip 里不含 Qt 运行库（`windeployqt` 不支持交叉部署）。`build.yml` 末尾附了备选方案说明。

### 构建矩阵里跑什么（三道闸门）

`build.yml` 的 Linux 两个 job 会依次跑完这三道闸门（Windows / 安卓 job 只负责出产物）：

1. **记法/解析器** — `tools/test_syntax`：每种写法一条用例，含旧格式兼容、容错/严格两档
2. **提示词与仓库内乐谱** — `tools/check_prompt` + `save --check`：严格解析、小节对齐、无削波/直流
3. **合成确定性** — `ci/check_metrics.sh` 与 `ci/metrics_golden.txt` 逐字段比对（跨平台/跨编译器一致性）

本地复刻：

```bash
cmake -S music_editor -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure          # 10 个用例, 约 6 秒
bash ci/check_metrics.sh build/save/save ci/metrics_golden.txt
python ci/validate_workflows.py                     # 校验 workflow 的 YAML 与本地引用
```

> 改了合成算法或乐器参数后，第 3 道闸门会失败，这是**预期行为**。确认改动无误后同步基准：
> ```bash
> for d in demos/*.rcp; do ./build/save/save "$d" --metrics | sed 's|^METRICS ||; s|\\|/|g; s|[^ ]*/||'; done > ci/metrics_golden.txt
> ```

### 出产物与发布

1. 手动触发 `build.yml`（Actions → Build Matrix → Run workflow，可选填版本号），或 `git push origin <tag>`
2. 矩阵并行构建全部目标，产物按上表命名，从对应 run 的 Artifacts 下载
3. 需要 GitHub Release 时手动创建：`gh release create <tag> <下载下来的产物>`
   （原先自动算版本号、打 tag 并发布 Release 的 `release.yml` 已移除；`ci/next_version.sh` 仍可本地试算版本号）

> 安卓 release APK 目前用 debug 签名，仅供内测分发；正式上架需自行配置 keystore（`android/app/build.gradle.kts` 的 `signingConfigs`）。

## 详细格式说明

完整记法（音符语法、`@` 元数据、物理模型、体检指标、迁移说明）见 **[`music_editor/readme.md`](music_editor/readme.md)**。

一句话速览：

```
# 注释以 "# " 开头
@ref A4=440          # 或 261.63 / C4 / 1=C
@bpm 96
@beat 0.625          # 标准拍长 = 60/BPM (一个四分音符的秒数)
@meter 4/4           # 仅用于小节对齐校验
@timbre harp         # 或 @acoustic 自定义物理参数
@voice 1             # 多声部: 每个 @voice 独立时间轴, 纵向叠加
6.04!f 7.04 #4.04 5.04_5.08 [1.02+3.02]_[1.02+3.02] r.04
```
