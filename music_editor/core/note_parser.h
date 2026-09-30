#ifndef MUSIC_NOTE_PARSER_H
#define MUSIC_NOTE_PARSER_H

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ── 物理乐器模型参数 ──────────────────────────────────────────────
// 详见 readme.md「物理模型」一节:
//   f_n = n·f1·sqrt(1 + B(f1)·n²)                   (劲度 → 非谐性, B 随音高变化)
//   A_n(0) = 频谱包络(n, f) · |sin(nπ·pos)| · 共鸣体(f_n) · 力度倾斜
//   τ_n 由阻尼律给出: tau_n = decay/n^exp  或  1/tau_n = 1/decay + n2·n²
//   包络   = 起音 + 稳态/衰减(可双指数) + 制音器(felt damper) + 自然余音

/// 共鸣体共振峰 (小提琴琴箱 / 钢琴音板 / 管体 ...)
/// 用模拟 biquad 峰值滤波器幅频响应实现, 与采样率无关
struct BodyResonance {
    double freq_hz = 1000.0;   ///< 共振中心频率
    double q       = 8.0;      ///< Q 值 (带宽)
    double gain_db = 0.0;      ///< 峰值增益 (dB)
};

/// 显式分音表条目: 钟/马林巴/定音鼓等非谐波乐器直接给比率
struct PartialSpec {
    double ratio = 1.0;   ///< 相对基频的比率
    double amp   = 1.0;   ///< 初始振幅 (相对值)
    double tau   = 2.0;   ///< 衰减时间常数 (秒)
};

/// 初相位模式
enum class PhaseMode : uint8_t {
    Coherent,   ///< 相干: 由激励点 sin(nπ·pos) 决定 0/π (击弦/拨弦的物理初相)
    Random,     ///< 随机 (确定性 hash), 峰值更低但起音更"散"
};

/// 阻尼律
enum class DampingLaw : uint8_t {
    PowerExp,   ///< 旧式: tau_n = decay / n^exp
    TwoTerm,    ///< 物理式: 1/tau_n = 1/decay + n2·n²  (空气黏滞 + 内摩擦)
};

/// 律制
enum class Temperament : uint8_t {
    Equal12,      ///< 十二平均律 (默认)
    Just,         ///< 纯律 (五度相生/自然音阶)
    Pythagorean,  ///< 毕达哥拉斯律
};

struct AcousticParams {
    std::string name      = "pure";

    // ── 分音结构 ────────────────────────────────────────────────
    double inharmonicity  = 0.0;      ///< B(C4); 理想弦=0; 吉他 1e-5; 钢琴 2e-4; 钟 3e-2
    double inharm_alpha   = 0.0;      ///< B(f) = B·(f/ref)^alpha (真实钢琴 1.5~2)
    double inharm_ref_hz  = 261.625565; ///< B 的参考音高
    double brightness     = 0.7;      ///< br, A_n(0) ∝ br^(n-1) (br 律)
    double tilt_db_oct    = 0.0;      ///< 源频谱倾斜 (dB/oct); >0 时按频率计算, 忽略 br
    double partial_floor  = 1e-4;     ///< 分音低于此振幅即停止生成 (-80 dB)
    int    max_partials   = 192;      ///< 分音数上限 (与 Nyquist 取小)

    // ── 激励点 (拨/击弦位置) ────────────────────────────────────
    double strike_pos     = 0.0;      ///< x/L; 0 = 不启用梳状零点; 钢琴 ≈0.125, 吉他 ≈0.2
    double strike_p       = 1.0;      ///< 2 = 理想拨弦的 1/n 包络 (更暗更木质)

    // ── 共鸣体 (音板/琴箱/管体) ─────────────────────────────────
    std::vector<BodyResonance> body;  ///< 共振峰列表
    double body_hp_hz     = 0.0;      ///< 低频滚降转折 (0 = 关闭)
    double body_hp_db_oct = 0.0;      ///< 转折以下的衰减斜率 (dB/oct, 正数)

    // ── 包络与衰减 ──────────────────────────────────────────────
    DampingLaw damping_law = DampingLaw::PowerExp;
    double decay          = 2.0;      ///< tau_1 (秒)
    double damping_exp    = 0.8;      ///< γ: tau_n = decay/n^γ (PowerExp)
    double damping_n2     = 0.0;      ///< n2: 1/tau_n = 1/decay + n2·n² (TwoTerm)
    double decay2_beta    = 0.0;      ///< 双指数第二段权重 β (0 = 关闭); 钢琴余韵 0.2~0.4
    double decay2_ratio   = 0.35;     ///< tau_fast = tau_slow·ratio
    double attack         = 0.004;    ///< 起音时间 (秒)
    double sustain        = 0.0;      ///< 稳态电平 0~1
    double release        = 0.02;     ///< 缓冲区末尾自然收尾 (秒), 保证不咔哒
    double damper         = 0.0;      ///< 制音器落下时间常数 (秒); 0 = 无制音器(自由余音)
    double ring           = 8.0;      ///< 无制音器乐器的自然余音上限 (秒)
    std::vector<double> partial_decay; ///< 逐分音 tau (秒), 非空则覆盖上面所有阻尼律

    // ── 起音噪声 (弓毛/气流/击弦) ───────────────────────────────
    double noise          = 0.0;      ///< 0~1
    double noise_len      = 0.012;    ///< 时长 (秒)

    // ── 力度耦合 (越用力越亮/越硬) ──────────────────────────────
    double vel_tilt       = 0.0;      ///< dB/oct · (1-v): pp 比 ff 暗多少
    double vel_attack     = 0.0;      ///< 0~1: 力度 0 时起音时间乘以 (1-vel_attack)
    double vel_noise      = 0.0;      ///< 0~1: 力度 0 时噪声量乘以 (1-vel_noise)

    // ── 空间 ────────────────────────────────────────────────────
    double stereo         = 0.0;      ///< 声像宽度 0~1 (与 pan/pan_pitch 叠加, 加宽立体声像)
    double pan            = 0.0;      ///< 固定声像 -1(左)~+1(右)
    double pan_pitch      = 0.0;      ///< 声像随音高展开 0~1 (钢琴键盘: 低音左)

    // ── 同音弦组 ────────────────────────────────────────────────
    int    unison         = 1;        ///< 同音弦数 (钢琴 2~3)
    double detune_cents   = 0.0;      ///< 弦间失谐 (音分) → 拍频闪烁

    PhaseMode phase       = PhaseMode::Coherent;

    /// 显式分音表 (非空则完全取代谐波列与 B)
    std::vector<PartialSpec> partial_table;

    // ── 律制 ────────────────────────────────────────────────────
    Temperament temperament = Temperament::Equal12;
    double stretch        = 0.0;      ///< 0~1: 按非谐性做伸展调音 (钢琴调律师的 Railsback 曲线)

    uint32_t set_mask     = 0;        ///< 内部: 记录哪些字段被显式给出 (见 AcousticField)
};

/// AcousticParams::set_mask 位标记
enum AcousticField : uint32_t {
    AF_INHARM  = 1u << 0,
    AF_DECAY   = 1u << 1,
    AF_EXP     = 1u << 2,
    AF_ATTACK  = 1u << 3,
    AF_SUSTAIN = 1u << 4,
    AF_RELEASE = 1u << 5,
    AF_BRIGHT  = 1u << 6,
    AF_NOISE   = 1u << 7,
    AF_STEREO  = 1u << 8,
    AF_TILT    = 1u << 9,
    AF_FLOOR   = 1u << 10,
    AF_MAXP    = 1u << 11,
    AF_INHALPHA= 1u << 12,
    AF_STRIKE  = 1u << 13,
    AF_BODY    = 1u << 14,
    AF_DAMP2   = 1u << 15,   ///< TwoTerm 阻尼律
    AF_DECAY2  = 1u << 16,   ///< 双指数余韵
    AF_DAMPER  = 1u << 17,
    AF_RING    = 1u << 18,
    AF_NOISELEN= 1u << 19,
    AF_VELTILT = 1u << 20,
    AF_UNISON  = 1u << 21,
    AF_PHASE   = 1u << 22,
    AF_TABLE   = 1u << 23,
    AF_TEMPER  = 1u << 24,
    AF_PAN     = 1u << 25,
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
    bool  has_pos = false; double pos = 0;      ///< $pos=  激励点
    bool  has_tilt= false; double tilt = 0;     ///< $tilt= 频谱倾斜 dB/oct
    bool  has_damper = false; double damper = 0;///< $damper= 制音器 tau
    bool  has_ring = false; double ring = 0;    ///< $ring= 余音上限
    bool  has_n2  = false; double n2 = 0;       ///< $n2=   双项阻尼
    bool  has_uni = false; int    uni = 1;      ///< $uni=  弦数
    bool  has_detune = false; double detune = 0;///< $detune= 弦间失谐(音分)
    bool  has_pan = false; double pan = 0;      ///< $pan=  声像
};

/// 连奏方式
enum class Articulation : uint8_t {
    Auto,      ///< 由乐器 sustain/damper 决定
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
    double velocity       = 1.0;   ///< 力度 (0~4, 1.0 = ff 基准; 来自 dB 映射表)
    Articulation articulation = Articulation::Auto;
    bool   token_start    = false; ///< 是否为所属 token 的第一个事件 (音块起点)
    NoteOverrides ov;              ///< 逐音物理覆盖
};

// ── 力度 (dB 语义) ────────────────────────────────────────────────
/// 力度名表: 名称 → 相对 ff 的 dB (ppp=-24 ... ff=0 ... fff=+3)
/// 返回 false 表示不是已知力度名
bool dynamic_db(std::string_view name, double& db_out);

/// dB → 线性振幅 (velocity), 限幅 [0,4]
double db_to_amplitude(double db);

/// 全部内置力度名 (供 UI 下拉/提示词使用)
const std::vector<std::string>& dynamic_names();

// ── 物理参数解析 ──────────────────────────────────────────────────
/// 解析 `@acoustic <名称> k=v;...` 行的参数部分 (忽略名称, name 由调用方设置)
/// 支持键见 readme.md; 未知键抛 std::invalid_argument
AcousticParams parse_acoustic_params(std::string_view kv);

/// 物理预设 (供 @acoustic 默认值/UI 下拉/示例曲目使用)
namespace Instruments {
    // 键盘
    AcousticParams piano();
    AcousticParams e_piano();
    AcousticParams harpsichord();
    AcousticParams music_box();
    // 拨弦
    AcousticParams guitar();
    AcousticParams guitar_nylon();
    AcousticParams bass_guitar();
    AcousticParams harp();
    AcousticParams guzheng();
    AcousticParams pipa();
    AcousticParams yangqin();
    // 弓弦
    AcousticParams violin();
    AcousticParams viola();
    AcousticParams cello();
    AcousticParams contrabass();
    AcousticParams erhu();
    // 管乐
    AcousticParams flute();
    AcousticParams dizi();
    AcousticParams clarinet();
    AcousticParams oboe();
    AcousticParams sax();
    AcousticParams trumpet();
    AcousticParams harmonica();
    AcousticParams organ();
    // 键盘打击 / 金属
    AcousticParams bells();
    AcousticParams gong();
    AcousticParams vibraphone();
    AcousticParams marimba();
    AcousticParams xylophone();
    AcousticParams timpani();
    AcousticParams taiko();
    // 电声 / 人声
    AcousticParams choir();
    AcousticParams synth_pad();

    /// 按名称查找 (不区分大小写), 找不到返回 nullptr
    const AcousticParams* find_by_name(std::string_view name);
    const std::vector<AcousticParams>& all();

    /// 乐器分类 (UI 分组用): "键盘" / "拨弦" / "弓弦" / "管乐" / "打击" / "电声"
    std::string category_of(std::string_view name);
}

// ── 简谱解析器 ────────────────────────────────────────────────────
/**
 * 音符格式: <音级>[变音].<八度><拍长分母>[修饰][!力度][@音色][$物理][~|']
 * 详见 readme.md。
 */
class NoteParser {
public:
    explicit NoteParser(double base_freq = 261.63,
                        double base_beat_duration = 0.5);

    /// 解析单个音符字符串 (不含和弦括号与连音 '_')
    [[nodiscard]] Note parse(std::string_view note_str) const;

    /// 解析一个完整 token: 支持 [和弦] 与 '_' 连音
    [[nodiscard]] std::vector<Note> parse_token(std::string_view token) const;

    /// 通过 BPM 设置标准拍长 (base_beat_duration = 60 / bpm)
    void set_bpm(double bpm);

    /// 绑定自定义力度表 (@dyn 名称=振幅), 生命周期由调用方保证 (可为 nullptr)
    void set_dyn_table(const std::unordered_map<std::string, double>* table) { dyn_ = table; }
    const std::unordered_map<std::string, double>* dyn_table() const { return dyn_; }

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
    const std::unordered_map<std::string, double>* dyn_ = nullptr;

    static const std::unordered_map<char, int> NOTE_TO_SEMITONE;
};

// ── 持续比例 (旧语法, 已 deprecated) ──────────────────────────────
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
    double bpm               = 120.0;
    double base_freq         = 261.63;
    double beat_duration     = 0.5;
    int    meter_num         = 4;
    int    meter_den         = 4;
    std::string timbre_name;
    bool   has_harmonics     = false;
    std::vector<double> harmonics;
    bool   has_sustain       = false;
    std::vector<HarmonicSustain> sustain;
    bool   has_acoustic      = false;
    AcousticParams acoustic;
    std::vector<Note> notes;
    std::vector<std::string> warnings;

    /// @dyn 自定义力度 (名称 → 线性振幅); 空则用内置 dB 表
    std::unordered_map<std::string, double> dyn;
};

RcpDocument parse_rcp(std::string_view content);
RcpDocument parse_rcp_strict(std::string_view content);

/// 拆分一行中的音符 token (识别 [和弦] 分组, 组内空格不分割)
std::vector<std::string> split_note_tokens(std::string_view line);

/// 按空白与括号拆分 token (同时产出括号信息, 供校验)
bool looks_like_note_line(std::string_view line);

/// 该行是否为 `@xxx` 元数据行
bool is_directive_line(std::string_view line);

// ── 兼容旧接口 ────────────────────────────────────────────────────
bool parse_rcp_header(std::string_view line,
                      double& bpm,
                      double& base_freq,
                      double& base_beat_duration);

std::vector<std::string> tokenize_notes(const std::vector<std::string>& lines);
std::vector<HarmonicSustain> parse_sustain_line(std::string_view line);
bool is_sustain_line(std::string_view line);
bool is_harmonics_line(std::string_view line);
std::vector<double> parse_harmonics_line(std::string_view line);

#endif // MUSIC_NOTE_PARSER_H
