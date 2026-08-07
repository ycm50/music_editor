# RCP 乐谱编辑器 (C++ 重构版)

基于简谱的音乐编辑和播放工具。原 Python 版本用 C++ 重构，修复了若干问题。

---

## 项目结构

```
music_editor/
├── CMakeLists.txt        # 根构建文件
├── core/                 # 共享库（纯 C++20，无需 Qt）
│   ├── note_parser.h/.cpp  # 简谱解析器
│   ├── tone_gen.h/.cpp     # 泛音合成 + 音色库
│   └── wav_writer.h/.cpp   # WAV 文件写入
├── player/               # 音乐播放器（需 Qt6::Multimedia）
├── save/                 # RCP→WAV 转换器（纯 C++，无 Qt）
├── ui/                   # GUI 编辑器（需 Qt6::Widgets）
├── 1.rcp                 # 乐谱数据文件
├── run_player.bat        # 播放器启动脚本（Windows）
├── run_ui.bat            # GUI 启动脚本（Windows）
└── README.md
```

## 依赖

- **编译器**: C++20 (GCC/MinGW 13+ / MSVC 2022+)
- **构建工具**: CMake ≥ 3.16, Ninja 或 MinGW Makefiles
- **Qt**: Qt 6.x (推荐) 或 Qt 5.15+
  - `player`: 需要 `Qt6::Multimedia` 模块
  - `ui`: 需要 `Qt6::Widgets` 模块
  - `save` 和 `core`: **不依赖 Qt**

## 构建

```bash
cd music_editor
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=A:/msys2/ucrt64
cmake --build build
```

> MSYS2 (UCRT64) 依赖: `mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-qt6-multimedia mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja`

### Windows 特殊说明

`player` 和 `ui` 依赖 Qt DLL。编译后运行：

```bash
# 方法一：使用启动脚本（自动设置路径）
run_player.bat 1.rcp --timbre piano
run_ui.bat

# 方法二：手动设置 PATH
set PATH=A:\msys2\ucrt64\bin;%CD%\build\player;%PATH%
build\player\player.exe 1.rcp --timbre piano
```

> `ui` 为 GUI 子系统（`WIN32_EXECUTABLE`），启动不弹终端窗口；它会自动在同目录或 `../` 下查找 `player.exe` / `save.exe`（发布包为同目录平铺）。

## 可执行文件

| 命令 | 对应原 Python 版 | 功能 |
|---|---|---|
| `player <file.rcp> [--timbre T]` | player.py | 播放乐谱 |
| `save <file.rcp> [--timbre T] [-o out.wav]` | save.py | 导出 WAV |
| `ui` | ui.py | 图形界面编辑器 |

> 注: 原 Python 版本 (`player.py` / `save.py` / `ui.py`) 已由 C++ 重写并移除, 上表"对应原 Python 版"仅用于对照功能来源。

参数 `--timbre` 可选: `piano`（默认）、`violin`、`flute`

> `ui` 与手机端一致: 单个编辑框直接编辑完整 RCP 内容 (头部/BPM/基准频率/拍长/音色/持续比例/音符), BPM 等参数从编辑框内容解析, 无需额外参数栏。内置 AI 生成谱 (OpenAI 兼容接口, 流式输出, 生成前自动清空编辑框), 含复制提示词按钮, 在 "设置 (AI)" 中配置 Base URL / API Key / 模型。
> 编辑内容与 AI 设置自动持久化到 `ui.exe` 同目录的 `config.json` (保存按钮或关闭窗口时写入), 下次启动自动恢复。

## RCP 文件格式 (统一格式, 桌面/手机共用)

```
BPM,基准频率,标准拍长
1,0.7,0.5,0.3,0.2,0.15,0.1      (可选) 音色行: 各倍频振幅, 逗号分隔
0-1!0-0.8!0-0.6!0-0.4!...       (可选) 持续比例行: 各倍频的持续区间, '!' 分隔
3.+1 2.+2 1.+1 2.+2
...
```

- **第一行**: `BPM,基准频率(Hz),标准拍长(秒)`（三个参数逗号分隔）
- **第二行(可选)**: 音色, 逗号分隔的各倍频振幅 (第 1 个为基频), 缺失时用 `--timbre` 指定或默认 piano
- **第三行(可选)**: 持续比例, 各倍频在音符时长内的存在区间, 用 `!` 分隔, 整体为 1:
  - `0-1` 全程存在, `0-0` 不存在(静音), `0.3-0.5` 只在 0.3~0.5 处出现
  - 多区间用花括号: `{0.1-0.3,0.5-0.7}`; 末尾 `>` 表示该区间内音量渐降到 0, 如 `{0.1-0.3>}`
- **后续行**: 空格分隔的音符字符串

### 音符格式

```
<音>.<八度><拍长分母>
```

| 部分 | 说明 | 例 |
|---|---|---|
| **音** | 1~7（唱名），0（休止符） | `3` |
| **八度** | `0`=中音, `-`=低八度, `+`=高八度, 可连续（`++`=高两个八度, `--`=低两个八度） | `0` |
| **拍长分母** | 4=四分, 2=二分, 8=八分, 1=全音, `:`代替`.`表连音 | `2` |

完整示例: `3.+2` = 3 的高八度二分音符；`1.++1` = 1 的高两个八度全音符

## 与原 Python 版相比的改进

| # | 问题 | 修复 |
|---|---|---|
| 1 | Python 列表缺逗号致字符串拼接 | C++ 版从 RCP 文件解析，无硬编码列表 |
| 2 | 注释写 120BPM 实际传 388 | 默认值取自 `1.rcp` 头部，代码文档统一 |
| 3 | WAV 导出无音符间隔 | `player` 和 `save` 均调用 `add_gap(0.05)` 加 50ms 间隔 |
| 4 | UI/save 两套音色不一致 | 统一音色表定义在 `core/tone_gen.h` |
| 5 | 未使用的 import | C++ 版无冗余引用 |
| 6 | 线程安全问题 | UI 通过 `QProcess` 启动外部进程播放，不存在竞态 |
| 7 | 播放只响一声 / 格式不被后端接受 | 渲染 float32，播放格式优先 `Float`、回退 `Int16`（兼容 FFmpeg/WASAPI 后端），分块写入避免缓冲溢出 |

## 音色

| 音色 | 谐波数 | 风格 |
|---|---|---|
| piano | 10 | 基频 + 逐渐衰减的谐波 |
| violin | 7 | 中高频丰富 |
| flute | 4 | 纯净柔和 |
