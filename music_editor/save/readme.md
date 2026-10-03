# `save` —— RCP 乐谱导出与校验工具

`save` 是 RCP 简谱工具链的命令行主力：**校验乐谱、乐理体检、导出 WAV**。
它不写盘就能做完整校验，因此适合放进 CI 当门槛。

- 源码：`music_editor/save/main.cpp`
- 输出：`build/save/save.exe`（Windows）
- 上层说明见 [项目主 README](../readme.md)

---

## 用法总览

```
save <file.rcp> [选项]
```

不带任何选项时，按谱内 `@timbre` 导出 WAV 到**当前工作目录**。

### 三个最常用的动作

| 目的 | 命令 |
|---|---|
| **校验乐谱**（不写盘，CI 用） | `save song.rcp --check` |
| **乐理/声学体检** | `save song.rcp --analyze --notes 20` |
| **导出音频** | `save song.rcp -o out.wav --bits 16` |

`--check` 通过时返回 **0**，失败返回非 0 —— 可直接作为 CI 断言。

---

## 选项参考

### 乐器 / 音色

| 选项 | 说明 |
|---|---|
| `-t, --timbre NAME` | 指定乐器；**仅在谱内未写 `@timbre` 时生效** |
| `-T, --force-timbre` | **强制覆盖**谱内音色（同谱做 A/B 音色对比） |
| `--list` | 列出全部乐器及其物理参数（按分类，共 33 种） |

> `@timbre` 是**文档级**指令：一个 `.rcp` 只能有一种音色。
> 想在同一首曲子里用多种音色，只能拆成多个文件后做音频层混音。

### 音色定制：`@acoustic`（写在谱里，非命令行选项）

除 33 种预设外，可用 `@acoustic` 改**物理建模参数**，造出预设之外的音色
（钟、锣、闷木等）。语法：

```
@acoustic <预设名> <键=值;键=值;...>
```

- **第一个 token 必须是预设名**（打底基音色）
- **多个参数用分号 `;` 分隔** —— 不是逗号，也不是空格
- 只覆盖显式给出的键，其余沿用预设

```text
@acoustic music_box b=0.05;damp=6.0;ring=12.0;tilt=3.0     ✅
@acoustic b=0.05;damp=6.0                                   ❌ 缺预设名
@acoustic music_box b=0.05,damp=6.0                         ❌ 用了逗号
@acoustic music_box b=0.05 damp=6.0                         ❌ 用了空格
```

> ⚠️ **报错信息有误导性，且三种错法报的错各不相同**（以下均为实测）：
>
> | 写法 | 实际报错 |
> |---|---|
> | `@acoustic b=0.05;damp=6.0`（缺预设名） | `unknown instrument in @acoustic: b=0.05;damp=6.0` |
> | `@acoustic music_box b=0.05,damp=6.0`（用逗号） | `invalid number for @acoustic b: 0.05,damp=6.0` |
> | `@acoustic music_box b=0.05 damp=6.0`（用空格） | `invalid number for @acoustic b: 0.05 damp=6.0` |
>
> 注意后两种报的是 **`b` 解析失败**（因为分隔符后整串被当成 `b` 的值），
> 而不是"分隔符用错了"——**报错指向的键名会误导排查方向**。
> 只有第一种才报 `unknown instrument`（看起来像乐器名写错，实际是**缺预设名**）。

#### 官方模板

`save --list` 末尾会直接印出全部可用键与一份可直接照抄的模板：

```text
自定义: @acoustic <名> B=2e-4;alpha=1.6;tilt=6;damp=2.5;n2=0.02;damper=0.09;pos=0.125;posp=1.6;
                    body=180:1.4:4,1400:1.2:3;hpf=85:6;uni=3;detune=0.9;
                    vtilt=5;velatk=0.45;beta=0.3;phase=coherent;table=1:1:2,4:0.4:0.5
```

模板里 `<名>` 可填已有预设名（打底）或任意新名（**纯自定义**，从零开始）：

```text
@acoustic music_box b=0.02;damp=5.0          # 以 music_box 为底, 只改两项
@acoustic my_bell b=0.05;damp=4.0;damper=0   # 全新音色, 不含任何预设特征
```

#### 完整参数表

| 键 | 含义 | 取值范围 |
|---|---|---|
| `B` / `b` | 非谐性（大 = 金属/钟感） | ≥ 0 |
| `alpha` | 非谐性随音高上升的指数 | ≥ 0 |
| `tilt` | 源频谱倾斜（dB/oct，小 = 亮，大 = 暖） | — |
| `damp` | 基频衰减时间常数（大 = 余音长） | > 0 |
| `n2` | 高次分音阻尼 | ≥ 0 |
| `damper` / `dmp` | 制音器时间常数 | ≥ 0，**0 = 自由余音** |
| `pos` | 激励点 x/L（拨/击弦位置，决定梳状零点） | [0, 0.5]，0.5 = 中央 |
| `posp` | 激励点指数 | [0, 4] |
| `body` | **琴体共振**，`f:q:db` 列表 | 如 `180:1.4:4,1400:1.2:3` |
| `hpf` | 高通滤波 `hz:斜率` | 斜率 ≥ 0 |
| `uni` / `detune` | 齐奏弦数 / 失谐（音分） | [1,8] / [0,50] |
| `vtilt` | 力度→音色倾斜 | ≥ 0 |
| `velatk` | 力度→起音 | [0, 1) |
| `velnoise` | 力度→噪声量 | [0, 1] |
| `beta` | 二次衰减比例 | [0, 1) |
| `ratio` | 二次衰减时长比 | (0, 1] |
| `phase` | 分音相位 `coherent` / `random` | — |
| `ring` | 余音上限（秒） | (0, 60] |
| `maxp` | 分音数上限 | [1, 1024] |
| `noiselen` | 噪声段长度 | (0, 1] |
| `atk` `dec` `sus` `rel` | ADSR | `sus` ∈ [0,1] |
| `br` | 亮度 | (0, 1] |
| `stereo` / `pan` / `panpitch` | 立体声展开 / 声像 / 声像随音高 | [0,1] / [−1,1] / [0,1] |
| `temp` / `stretch` | 律制 / 伸展调音 | 12tet\|just\|pyth / [0,1] |
| `table` | **任意分音表** `ratio:amp:tau` 列表 | 见下 |

#### `table=` —— 绕过物理模型，直接指定分音

这是最强的一个键：不走物理建模，**逐个指定分音的频率比、幅度、衰减时间**，
相当于任意加法合成。

```text
table=1:1:2,4:0.4:0.5      # 分音1: 1倍频/幅度1/衰减2s
                           # 分音2: 4倍频/幅度0.4/衰减0.5s
```

配合 `@acoustic my_name ...` 把 `my_name` 当纯容器用，就能造出物理模型
给不了的音色（无泛音列、非谐分音、特定拍频等）。

> ⚠️ **`table=` 与 `$dec=`**：分音表非空时，`--analyze` 报告的 `T30` 取自表内
> 首项的 `tau`，**逐音 `$dec=` 会被完全绕过**（`tone_gen.cpp:1093`）。
> 这类音色要调衰减请改 `table` 里的 `tau` 或换用 `$damper=`。

**验证是否生效**：改完用 `--analyze` 对比 `T30` 与频谱重心。
实测把 `music_box` 改为 `b=0.05;damp=6.0;ring=12.0;tilt=3.0`：

| 指标 | 预设基线 | 定制后 |
|---|---|---|
| T30 | 5.25 s | **9.07 s** |
| 发声时长 | 8.00 s | **12.00 s** |
| 频谱重心 | 4589 Hz | **6666 Hz** |

> `@acoustic` 是**文档级**（改整个文件的音色模型）；
> `$damper=` 等 `$参数` 是**逐音级**（音内后缀，改单个音）。两者可叠加。

### 输出

| 选项 | 说明 |
|---|---|
| `-o, --output FILE` | 输出 WAV 路径 |
| `--mono` | 单声道（默认输出立体声 + 混响） |
| `--no-reverb` | 关闭混响 |
| `--sample-rate N` | 采样率（默认 44100） |
| `--bits 16\|24` | 位深（24-bit 无量化失真） |
| `--no-dither` | 关闭 16/24-bit 的 TPDF dither |

> ⚠ **不会自动创建输出目录**。`-o out\x.wav` 在 `out\` 不存在时直接失败并返回 1，
> 而 `--check` 不写盘、发现不了这个问题。渲染前先 `mkdir out`。

> ⚠ **不写 `-o` 时输出到「当前工作目录」**。若在工具目录里执行，会堆出
> `output_<谱名>_<音色>.wav`。建议先切到临时目录，或始终显式给 `-o`。

### 混响

| 选项 | 默认 | 说明 |
|---|---|---|
| `--reverb-rt60 S` | 1.2 | 混响 RT60（秒） |
| `--reverb-pre MS` | 12 | 预延迟（毫秒） |
| `--reverb-width W` | — | 立体声宽度 0~1 |
| `--reverb-damp HZ` | 4000 | 高频吸收转折频率 |

### 归一化 / 响度

| 选项 | 说明 |
|---|---|
| `--normalize peak\|lufs\|none` | 归一化方式（**默认 `peak`**） |
| `--target X` | `peak` 模式的目标峰值（默认 0.92） |
| `--lufs X` | `lufs` 模式的目标响度（默认 −14 LUFS） |
| `--duration-comp` | 短音能量补偿（时域整合等响） |
| `--no-hold` | 关闭自然余音，严格按记谱时值截断（旧行为） |

### 律制 / 调音

| 选项 | 说明 |
|---|---|
| `--temperament 12tet\|just\|pyth` | 十二平均律 / 纯律 / 毕达哥拉斯律 |
| `--stretch X` | 伸展调音 0~1（钢琴 Railsback 曲线） |

### 分析 / 校验

| 选项 | 说明 |
|---|---|
| `--analyze` | 乐理/声学体检报告（含逐音明细） |
| `--notes N` | 逐音明细条数（配合 `--analyze`） |
| `--check` | **严格校验**：解析 + 小节对齐 + 削波/直流断言，**不写出 WAV** |
| `-q, --quiet` | 只输出关键结果 |
| `--metrics` | 输出一行规范化指标，供跨平台一致性比对 |

日志与提示信息输出到 **stderr**，因此 `--metrics` 的一行结果可以直接被脚本消费。

---

## `--check` 校验什么

`--check` 是最值得放进流水线的动作。它做**严格模式解析**并断言：

- 记法是否合法（含八度记号、和弦写法、力度记号）
- 每小节拍数是否与 `@meter` 对齐
- 导出后是否有**削波**样本
- 是否有**直流偏移**

不写盘，因此速度远快于渲染，也**不会**暴露"输出目录不存在"这类渲染期问题。

```bash
save song.rcp --check -q        # 静默模式，适合批量
echo $?                          # 0 = 通过
```

批量校验（PowerShell）：

```powershell
Get-ChildItem *.rcp | %{ & save $_.Name --check -q; "exit=$LASTEXITCODE" }
```

---

## `--analyze` 体检报告

`--analyze` 打印乐理与声学指标，**是核对音高的唯一可靠手段**——
不要凭记法直觉推断实际音高。

```bash
save song.rcp --analyze --notes 9999
```

逐音明细每行含：**音名** / 记谱秒 / 实际发声时长 / 力度 / 分音数 / **T30** 等。

配合 `Select-String` 可快速核对调性（应只出现调内音级）：

```powershell
& save song.rcp --analyze --notes 9999 |
  Select-String "^\s+\d+\s" | %{ ($_.ToString().Trim() -split '\s+')[1] } |
  Sort-Object -Unique
```

> 若出现不该有的升号（如小调曲子里冒出 `C#`），说明记法写错了：
> 本格式的**音级是固定大调唱名**，`3` 恒为**大三度**、`7` 恒为**大七度**，
> 写自然小调必须用 `b3` / `b6` / `b7`。

### 解析警告会被静默跳过

`--analyze` 中以 `!` 开头的行是**解析警告**。注意这类问题
**`--check` 仍可能返回 0**，例如：

```text
! 第 7 行 token "$damper=0.02" 已跳过: invalid note symbol in "$damper=0.02"
```

逐音 `$` 参数（如 `$dec=` / `$damper=`）**必须写成音内后缀**，
紧跟音符后面；单独占一行会被当作非法音符**整行跳过**：

```text
1.-01$damper=0.50      ✅ 合法，参数生效
$damper=0.50           ❌ 独立成行 → 整行被跳过
```

多个参数用 `/` 分隔（`,` 留给和弦成员）：

```text
1.-01$dec=0.3/sus=0.5
```

参数名支持别名：`$dec` = `$decay`、`$damper` = `$dmp`。

---

## 常见组合

```bash
# CI 门槛：批量严格校验
save song.rcp --check -q

# 高保真导出（24-bit、关闭 dither、纯律、钢琴伸展调音）
save song.rcp -o out.wav --bits 24 --no-dither --temperament just --stretch 1

# 流媒体响度（-14 LUFS）
save song.rcp -o out.wav --normalize lufs --lufs -14

# 同谱音色 A/B（用 -T 覆盖谱内 @timbre）
save song.rcp -T violin  -o a.wav
save song.rcp -T erhu    -o b.wav

# 干声（关混响，便于后期）
save song.rcp -o dry.wav --mono --no-reverb
```

---

## 已知限制

1. **一个 `.rcp` 只能有一种音色**（`@timbre` 文档级）。
   多乐器合奏需拆成多个文件，分别导出后再做音频层混音。
2. **弦乐类是单件乐器建模**，不是真实弦乐群。
3. **打击乐没有架子鼓预设**，需用 `taiko` / `xylophone` 等近似，
   并用 `$damper=` 拉开长短。
4. **`$dec=` 对「显式分音表」乐器无效**（`taiko` `xylophone` `bells`
   `gong` `timpani` `marimba` `vibraphone`），这类乐器要用 `$damper=` 控制衰减。
   该问题**静默发生**，`--check` 仍返回 0。
