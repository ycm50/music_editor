# RCP 简谱编辑器

基于简谱（数字谱）的音乐编辑与播放工具，单仓库同时提供 **桌面端**（C++20 + Qt6）与 **安卓端**（Kotlin + C++/JNI）。两端共用同一套纯 C++ 核心库完成解析、泛音合成与 WAV 编码，保证行为一致。

## 功能

- **RCP 乐谱**：自有文本谱格式，头部定义 `BPM,基准频率,标准拍长`，后续为空格分隔的音符
- **泛音合成**：内置三种音色（piano / violin / flute），支持自定义谐波振幅
- **导出 WAV**：RCP → 16-bit PCM 单声道 WAV
- **实时播放**：桌面端 Qt Multimedia（自动选择设备支持的音频格式），安卓端 AudioTrack
- **AI 生成**（安卓端）：OpenAI 兼容接口，一键生成乐谱与自定义音色
- **一键发布**：GitHub Actions 手动触发，产出 Windows zip、Ubuntu .deb、Android APK 并发布 Release

## 项目结构

```
.
├── music_editor/                   桌面端 (C++20 + CMake + Qt6)
│   ├── core/                       纯 C++ 共享库 (无 Qt 依赖, 桌面/安卓复用)
│   │   ├── note_parser             RCP 音符解析
│   │   ├── tone_gen                谐波合成 + 音色库
│   │   └── wav_writer              16-bit PCM WAV 编码/写入
│   ├── player/                     命令行播放器 (Qt6::Multimedia)
│   ├── save/                       RCP→WAV 转换器 (纯 C++, 无 Qt)
│   └── ui/                         GUI 编辑器 (Qt6::Widgets, GUI 子系统无终端窗口)
├── android/                        安卓端 (Kotlin + NDK/JNI)
│   └── app/src/main/cpp/           JNI 桥接, 直接引用 music_editor/core 源码
└── .github/workflows/release.yml   CI 构建 + 发布
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
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=A:/msys2/ucrt64
cmake --build music_editor/build --parallel
```

### 运行

`player` / `ui` 需要 Qt DLL 在 PATH 中（MSYS2 为 `A:\msys2\ucrt64\bin`）。发布包已内置全部 DLL，直接运行即可。

```bash
# 命令行播放
build/player/player.exe 1.rcp --timbre piano
# 导出 WAV
build/save/save.exe 1.rcp --timbre violin -o out.wav
# 图形界面 (无终端窗口; 自动查找同目录或 ../ 下的 player/save)
build/ui/ui.exe
```

`ui` 打包后为单目录平铺（`ui.exe`、`player.exe`、`save.exe` 及 Qt DLL 同目录）；开发目录布局（`build/ui`、`build/player`、`build/save` 分目录）同样支持。

## 安卓端

```bash
cd android
./gradlew :app:assembleRelease          # 或 assembleDebug
```

- C++ 核心经 JNI 暴露 `getTimbres` / `getTimbreHarmonics` / `renderPcm` / `renderWav`
- 编辑、播放（AudioTrack）、导出 WAV 到下载目录、导入 RCP
- AI 生成谱与音色：设置页配置 OpenAI 兼容 `base_url` / `api_key` / `model`

## CI 构建与发布

`.github/workflows/release.yml`（手动触发）：

1. 自动计算下一版本号（从 git tag 递增）
2. `desktop-win`：Windows runner + Qt `win64_mingw` + 同源 MinGW 13.1 编译，`windeployqt` 部署，产出 zip
3. `desktop-linux`：Ubuntu 构建，打包 `.deb`（声明 Qt 运行依赖）
4. `android`：assembleRelease（debug 签名）
5. 打 tag 并创建 GitHub Release，附带三平台产物

> 注：Windows 构建用与 Qt 同源的 MinGW 13.1（`tools_mingw1310`），避免 exe 与 Qt 的 libstdc++ 版本不一致导致启动失败；发布包显式拷贝三个可执行文件并做完整性校验。

## RCP 文件格式

```
BPM,基准频率,标准拍长
3.+1 2.+2 1.+1 2.+2
...
```

- **第一行**：`BPM,基准频率(Hz),标准拍长(秒)`，三个参数逗号分隔
- **后续行**：空格分隔的音符字符串

### 音符格式

```
<音>.<八度><拍长分母>
```

| 部分 | 说明 | 例 |
|---|---|---|
| **音** | 1~7（唱名），0（休止符） | `3` |
| **八度** | `0`=中音, `-`=低八度, `+`=高八度（必须写） | `0` |
| **拍长分母** | 4=四分, 2=二分, 8=八分, 1=全音, `:` 代替 `.` 表连音 | `2` |

完整示例：`3.+2` = 3 的高八度二分音符

## 音色

| 音色 | 谐波数 | 风格 |
|---|---|---|
| piano | 10 | 基频 + 逐渐衰减的谐波 |
| violin | 7 | 中高频丰富 |
| flute | 4 | 纯净柔和 |

## 详细文档

桌面端细节见 [`music_editor/readme.md`](music_editor/readme.md)。
