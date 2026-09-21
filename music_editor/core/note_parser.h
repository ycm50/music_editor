#ifndef MUSIC_NOTE_PARSER_H
#define MUSIC_NOTE_PARSER_H

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ── 物理乐器模型参数 ──────────────────────────────────────────────
// 详见 readme.md「物理模型」一节:
//   f_n = n·f1·sqrt(1 + B·n²)                  (劲度 → 非谐性)
//   A_n(t) = A_n(0)·(sustain + (1-sustain)·e^(-t/tau_n)),  tau_n = decay/n^exp
struct AcousticParams {
    std::string name      = "pure";
    double inharmonicity  = 0.0;    ///< B, 理想弦=0; 吉他 1e-5; 钢琴 1e-4~1e-3
    double decay          = 2.0;    ///< tau_1 (秒), 绝对时间而非时长比例
    double damping_exp    = 0.8;    ///< γ, tau_n = decay / n^γ
    double attack         = 0.004;  ///< 起音时间 (秒)
    double sustain        = 0.0;    ///< 稳态电平 0~1 (0=纯衰减, 弓/气流类 >0)
    double release        = 0.02;   ///< 谱面结束后的自然收尾 (秒)
    double brightness     = 0.7;    ///< br, A_n(0) ∝ br^(n-1)
    double noise          = 0.0;    ///< 起音噪声量 0~1
    double stereo         = 0.0;    ///< 分音左右微扩散 0~1
    std::vector<double> partial_decay;  ///< 逐分音 tau (秒), 非空则覆盖 decay/exp
    uint32_t set_mask     = 0;      ///< 内部: 记录哪些字段被显式给出 (见 AcousticField)
};

/// AcousticParams::set_mask 位标记
enum AcousticField : uint32_t {
    AF_INHARM = 1u << 0,
    AF_DECAY  = 1u << 1,
    AF_EXP    = 1u << 2,
    AF_ATTACK = 1u << 3,
    AF_SUSTAIN= 1u << 4,
    AF_RELEASE= 1u << 5,
    AF_BRIGHT = 1u << 6,
    AF_NOISE  = 1u << 7,
    AF_STEREO = 1u << 8,
};

/// 把 override 中显式给出的字段合并进 base (未给出的保留 base 原值)
void merge_acoustic(AcousticParams& base, const AcousticParams& override_params);

/// 单个音符的逐音覆盖 (RCP token 内 `$` 后缀)
struct NoteOverrides {
    bool  has_atk = false; double atk = 0;
    bool  has_dec = false; double dec = 0;
    bool  has_sus = false; double sus = 0;
    bool  has_rel = false; double rel = 0;
    bool  has_br  = false; double br  = 0;
    bool  has_B   = false; double B   = 0;
};

/// 连奏方式
enum class Articulation : uint8_t {
    Auto,      ///< 由乐器 sustain 决定 (衰减型自然分离, 持续型自然相连)
    Legato,    ///< 强制与下一音相连 (不额外断句)
    Staccato,  ///< 强制缩短到 gate 比例
};

/// 解析后的单个发声事件
struct Note {
    double frequency      = 0.0;   ///< 基频 (Hz); 休止符时为 0
    double duration_sec   = 0.0;   ///< 发声时长 (秒, 连音已合并)
    bool   rest           = false; ///< true = 休止符 (渲染为真静音)
    int    scale_degree   = 0;     ///< 1~7, 休止为 0 (供 --analyze 报告)
    int    accidental     = 0;     ///< 半音偏移: #=+1 b=-1 ##=+2 bb=-2
    int    octave_offset  = 0;     ///< 八度偏移: +=高, -=低
    double velocity       = 1.0;   ///< 力度 (0~1, 来自动态映射表)
    Articulation articulation = Articulation::Auto;
    bool   token_start    = false; ///< 是否为所属 token 的第一个事件 (音块起点)
    NoteOverrides ov;              ///< 逐音物理覆盖
};

// ── 物理参数解析 ──────────────────────────────────────────────────
/// 解析 `@acoustic <名称> k=v;...` 行的参数部分
/// 支持的键: B, damр/decay, exp, atk, sus, rel, br, noise, stereo
///   damр=0.8,0.6,0.4  逐分音 tau (秒)
///   damр=2.4/0.8      tau_1=2.4s, tau_n=tau_1/n^0.8
/// @throws std::invalid_argument 无法识别的键或非法数值
AcousticParams parse_acoustic_params(std::string_view kv);

/// 物理预设 (供 @acoustic 默认值/UI 下拉/示例曲目使用)
namespace Instruments {
    AcousticParams piano();
    AcousticParams violin();
    AcousticParams flute();
    AcousticParams guitar();
    AcousticParams harp();
    AcousticParams bells();
    AcousticParams music_box();
    AcousticParams organ();
    /// 按名称查找 (不区分大小写), 找不到返回 nullptr
    const AcousticParams* find_by_name(std::string_view name);
    const std::vector<AcousticParams>& all();
}

// ── 简谱解析器 ────────────────────────────────────────────────────
/**
 * 音符格式: <音级>[变音].<八度><拍长分母>[修饰][!力度][@音色][$物理][~|']
 *
 *   音级   : 1~7 唱名, 0 或 r 休止
 *   变音   : # 升, b 降, ## 重升, bb 重降 (全音域支持)
 *   八度   : 0=中音, +=高八度, -=低八度, 可连续 (++=高两个八度, 上限 4)
 *   拍长分母: 4=四分, 2=二分, 8=八分, 1=全音符, 16=十六分
 *            附点 '3.'=附点二分音符(×1.5); 三连音 '4t'=1/3 拍
 *   连音   : '_' 连接相邻同音, 时长相加且只起音一次 (1.04_1.04 = 二分音符)
 *   和弦   : [1.04+3.04+5.04] 同一起音时刻同时发声
 *
 * 例: "3.+2"      → 3 的高八度二分音符
 *     "#4.+8"     → 升 4 高八度八分音符
 *     "5.+2!ff"   → 强奏
 *     "1.+4$dec=0.3" → 该音衰减时间常数 0.3s
 *     "r.04"      → 真静音一拍
 */
class NoteParser {
public:
    explicit NoteParser(double base_freq = 261.63,
                        double base_beat_duration = 0.5);

    /// 解析单个音符字符串 (不含和弦括号与连音 '_')
    /// @throws std::invalid_argument 语法错误
    [[nodiscard]] Note parse(std::string_view note_str) const;

    /// 解析一个完整 token: 支持 [和弦] 与 '_' 连音
    /// @return 一个或多个 Note (和弦返回多个); 连音已合并为单个 Note
    /// @throws std::invalid_argument 语法错误
    [[nodiscard]] std::vector<Note> parse_token(std::string_view token) const;

    /// 通过 BPM 设置标准拍长 (base_beat_duration = 60 / bpm)
    void set_bpm(double bpm);

    // Setters
    void set_base_freq(double freq) { base_freq_ = freq; }
    void set_base_beat_duration(double dur) { base_beat_duration_ = dur; }

    // Getters
    double base_freq() const { return base_freq_; }
    double base_beat_duration() const { return base_beat_duration_; }

    /// 音级 + 八度 + 变音 → 频率 (Hz); 休止符返回 0
    double frequency_of(int scale_degree, int octave_offset, int accidental) const;

private:
    double base_freq_;           ///< 基准频率 (C4 = 261.63Hz, 即 1=do)
    double base_beat_duration_;  ///< 标准拍长 (四分音符时值, 秒)

    static const std::unordered_map<char, int> NOTE_TO_SEMITONE;
};

// ── 持续比例 (旧语法, 已 deprecated) ──────────────────────────────
/// 单个倍频的持续区间 (相对**绝对值区间**, 0~1 表示音符时长比例)
struct SustainRegion {
    double start    = 0.0;
    double end      = 1.0;
    bool   fade_out = false;
};

struct HarmonicSustain {
    std::vector<SustainRegion> regions;
};

// ── RCP 文档解析 ──────────────────────────────────────────────────

/// 解析 RCP 完整内容得到的结构化文档
struct RcpDocument {
    double bpm               = 120.0;   ///< 头部声明 (仅信息, 时值以 beat_duration 为准)
    double base_freq         = 261.63;  ///< 1=do 的频率 (Hz)
    double beat_duration     = 0.5;     ///< 标准拍长 = 一个四分音符的秒数
    int    meter_num         = 4;       ///< 拍号分子 (0 = 未声明)
    int    meter_den         = 4;       ///< 拍号分母 (0 = 未声明)
    std::string timbre_name;            ///< @timbre 行的音色名 (可空)
    bool   has_harmonics     = false;   ///< 是否显式给出谐波振幅表
    std::vector<double> harmonics;      ///< 第 2 行: 各倍频振幅 (原始比例)
    bool   has_sustain       = false;   ///< 是否显式给出持续比例行
    std::vector<HarmonicSustain> sustain; ///< 第 3 行: 各倍频时间窗 (需先声明)
    bool   has_acoustic      = false;   ///< 是否有 @acoustic 物理模型
    AcousticParams acoustic;            ///< @acoustic 解析结果
    std::vector<Note> notes;            ///< 所有声部的发声事件 (纵向叠加)
    std::vector<std::string> warnings;  ///< 非致命问题 (旧语法/数据异常)
};

/**
 * 解析 RCP 完整内容
 *
 * 头部第 1 行兼容两种写法:
 *   "BPM,基准频率,标准拍长"   旧写法 (BPM 与拍长冲突时以拍长为准并记 warning)
 *   "@bpm 388" / "@ref 261.63" / "@beat 0.2"   新写法 (可混用)
 *
 * 元数据行 (带 @ 前缀, 顺序无关, 均可用多次):
 *   @meter 4/4        拍号 (仅用于 --analyze 的乐理校验)
 *   @timbre piano     乐器名 (对应 Instruments 预设)
 *   @acoustic <名> k=v;...  物理模型参数
 *   @dyn mf=0.6,p=0.3 自定义动态映射
 *   @voice 1 / @voice 2 ...  多声部标记 (事件纵向叠加)
 *
 * 无前缀时的兼容识别: 第 2 行若为"逗号分隔非负数字"且**不是音符行**则视为
 * 谐波振幅; 第 3 行若能被 sustain 解析器接受则视为持续比例。
 *
 * @throws std::runtime_error 头部缺失/无法解析
 * @throws std::invalid_argument 音符语法错误
 */
RcpDocument parse_rcp(std::string_view content);

/**
 * 严格模式解析: 任何 token 语法错误直接抛 std::invalid_argument (附带行号)
 *
 * 默认的 parse_rcp 是**容错**的 —— 单个 token 写错会记入 warnings 并跳过,
 * 其余音符照常渲染 (避免一个笔误让整份谱打不开)。需要校验输入时用这个版本,
 * UI 的"检查乐谱"与自动化测试应该用它。
 */
RcpDocument parse_rcp_strict(std::string_view content);

/// 拆分一行中的音符 token (识别 [和弦] 分组, 组内空格不分割)
std::vector<std::string> split_note_tokens(std::string_view line);

/// 按空白与括号拆分 token (同时产出括号信息, 供校验)
bool looks_like_note_line(std::string_view line);

/// 该行是否为 `@xxx` 元数据行
bool is_directive_line(std::string_view line);

// ── 兼容旧接口 ────────────────────────────────────────────────────
/// 解析 RCP 头部 (旧接口, 单行 "BPM,base_freq,beat")
bool parse_rcp_header(std::string_view line,
                      double& bpm,
                      double& base_freq,
                      double& base_beat_duration);

/// 将多行音符内容分词为 token 列表 (旧接口)
std::vector<std::string> tokenize_notes(const std::vector<std::string>& lines);

/// 解析持续比例行 (旧语法), 各项用 '!' 分隔
std::vector<HarmonicSustain> parse_sustain_line(std::string_view line);

/// 是否为持续比例行 (旧语法)
bool is_sustain_line(std::string_view line);

/// 是否为"逗号分隔非负数字"行 (旧语法音色行)
bool is_harmonics_line(std::string_view line);

/// 解析逗号分隔的振幅列表
std::vector<double> parse_harmonics_line(std::string_view line);

#endif // MUSIC_NOTE_PARSER_H
