#ifndef MUSIC_TONE_GEN_H
#define MUSIC_TONE_GEN_H

#include "note_parser.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// ── 内置音色 (旧接口: 裸谐波振幅表) ───────────────────────────────
struct Timbre {
    std::string name;
    std::vector<double> harmonics; ///< 各次谐波振幅系数 [基频, 2次, 3次, ...]
};

/// 预定义音色库 (旧的裸谐波表; 新代码请用 Instruments 物理模型)
namespace Timbres {
    inline const Timbre PIANO{
        "piano",
        {1.0, 0.7, 0.5, 0.3, 0.2, 0.15, 0.1, 0.08, 0.05, 0.03}
    };

    inline const Timbre VIOLIN{
        "violin",
        {1.0, 0.7, 0.5, 0.3, 0.2, 0.15, 0.1}
    };

    inline const Timbre FLUTE{
        "flute",
        {1.0, 0.2, 0.1, 0.05}
    };

    const Timbre* find_by_name(std::string_view name);
    const std::vector<Timbre>& all();
}

// ── 归一化方式 ────────────────────────────────────────────────────
enum class NormMode : uint8_t {
    Peak,   ///< 峰值归一 (默认, 只缩不放)
    Lufs,   ///< 响度归一 (简化 BS.1770), 跨谱子音量可比
    None,   ///< 不归一化 (原始电平)
};

// ── 渲染选项 ──────────────────────────────────────────────────────
struct RenderOptions {
    int  sample_rate      = 44100;
    bool stereo           = false;  ///< true = 输出立体声 (左右交错)
    // 混响 (Schroeder 反馈网络, 参数化)
    double reverb_wet     = 0.28;   ///< 湿量 0~1 (0 = 关闭)
    double reverb_rt60    = 1.2;    ///< 目标 RT60 (秒)
    double reverb_predelay_ms = 12.0; ///< 预延迟 (ms)
    double reverb_damp_hz = 4000.0; ///< 高频吸收转折 (Hz, 0 = 不衰减高频)
    double reverb_width   = 1.0;    ///< 立体声宽度 0~1
    // 电平
    NormMode normalize    = NormMode::Peak;
    double normalize_target = 0.92; ///< peak 模式的目标峰值
    double lufs_target    = -14.0;  ///< lufs 模式的目标响度 (LUFS)
    bool   limit          = true;   ///< 真峰值保护 (软限幅)
    bool   dur_compensate = false;  ///< 短音等响补偿 (时域整合 < 200ms 的响度损失)
    bool   hold_last_note = true;   ///< 末音按乐器自然余音延展 (不再被截断)
};

// ── 渲染结果与体检指标 ────────────────────────────────────────────
/// 单个发声事件的物理参数报告 (供 --analyze 输出)
struct NotePhysics {
    int    index         = 0;
    double frequency     = 0.0;   ///< 基频 (Hz)
    double duration_sec  = 0.0;   ///< 记谱时值 (gate)
    double ring_sec      = 0.0;   ///< 实际发声长度 (含余音)
    double velocity      = 1.0;
    bool   rest          = false;
    int    partials      = 0;     ///< 参与合成的分音数 (模型)
    int    discarded     = 0;     ///< 越过 Nyquist 被丢弃的分音数
    double centroid_hz   = 0.0;   ///< 模型频谱重心, Σ A_n·f_n / Σ A_n
    double meas_centroid_hz = 0.0;///< 渲染音频实测频谱重心 (Goertzel)
    int    meas_partials = 0;     ///< 实测 (> -60dB) 分音数
    double tau1_sec      = 0.0;   ///< 第 1 分音衰减时间常数
    double t30_sec       = 0.0;   ///< 实测衰减: 由 -5dB 到 -35dB 折算出 T30
    double attack_sec    = 0.0;   ///< 实测起音时间 (10% → 90%)
    bool   truncated     = false; ///< 结束时仍在发声 (被截断 → 咔哒风险)
    double b_used        = 0.0;   ///< 该音实际使用的非谐性 B(f)
    double pan           = 0.0;   ///< 实际声像 (-1..1)
    double tonic_cents   = 0.0;   ///< 相对基准频率的音分偏差 (含律制/伸展)
};

/// 整曲体检报告 (--analyze)
struct RenderStats {
    double duration_sec   = 0.0;
    size_t total_samples  = 0;    ///< 单声道样本数
    double peak           = 0.0;
    double true_peak      = 0.0;  ///< 过采样估计真峰值
    double lufs           = 0.0;  ///< 响度 (简化 BS.1770)
    double rms            = 0.0;
    double dc_offset      = 0.0;
    double crest_db       = 0.0;
    double silence_ratio  = 0.0;
    double clipped_ratio  = 0.0;
    double max_jump       = 0.0;  ///< 相邻样本最大跳变 (参考值; 带限信号本身也可能很大)
    double max_jump_at    = 0.0;  ///< 出现时刻 (秒)
    double click_ratio    = 0.0;  ///< 最大 |Δx| / 局部平均 |Δx| (孤立尖峰 → 硬切/咔哒)
    double click_at       = 0.0;  ///< 尖峰时刻 (秒)
    int    boundary_glitches = 0; ///< 起止样本非零的音块数 (硬切 → 咔哒的唯一来源)
    int    note_count     = 0;
    int    rest_count     = 0;
    int    chords         = 0;
    int    max_polyphony  = 0;
    int    discarded_total = 0;
    int    truncated_notes = 0;   ///< 被截断的发声事件数
    int    partials_min   = 0;    ///< 分音数范围 (体现低/高音区差异)
    int    partials_max   = 0;
    double ring_min_sec   = 0.0;
    double ring_max_sec   = 0.0;
    double t30_min_sec    = 0.0;
    double t30_max_sec    = 0.0;
    double centroid_ratio_min = 0.0; ///< 实测重心/f0 的最小值
    double centroid_ratio_max = 0.0; ///< 实测重心/f0 的最大值 (音色随音区变化)
    double bar_sec        = 0.0;
    double bars           = 0.0;
    double bar_fit_error  = 0.0;
    double freq_min       = 0.0;
    double freq_max       = 0.0;
    std::string instrument;
    std::vector<std::string> warnings;
    std::vector<NotePhysics> notes;
};

// ── 分音表 (合成与体检共用) ───────────────────────────────────────
struct TonePartial {
    double freq     = 0.0;   ///< 分音频率 (Hz, 含非谐性)
    double amp      = 0.0;   ///< 初始振幅 (未归一)
    double tau      = 0.0;   ///< 衰减时间常数 (秒)
    bool   inverted = false; ///< 相干相位取 π (由激励点梳状决定)
};

/// 逐音覆盖合并到乐器参数
AcousticParams resolve_acoustic(const AcousticParams& base, const NoteOverrides& ov);

/// 按物理模型生成分音表: 激励点梳状 × 频谱倾斜(br) × 共鸣体 × 力度倾斜,
/// 分音数由 f0 与 Nyquist/听阈决定 (低音分音多, 高音分音少)
std::vector<TonePartial> build_partials(double f0,
                                        const AcousticParams& a,
                                        int sample_rate,
                                        double velocity = 1.0);

/// 律制/伸展调音: 把 12-TET 频率按律制与 Railsback 伸展修正
double apply_temperament(double freq_12tet, double tonic_hz,
                         Temperament temperament, double stretch);

// ── 单音实测指标 ──────────────────────────────────────────────────
struct ToneAnalysis {
    double attack_sec   = 0.0;
    double t30_sec      = 0.0;
    double centroid_hz  = 0.0;
    int    partials     = 0;
    bool   truncated    = false;
};

/// 分析单音缓冲区 (起音时间 / T30 / 实测频谱重心 / 分音数 / 是否被截断)
ToneAnalysis analyze_tone(const std::vector<float>& tone, int sample_rate,
                          const std::vector<double>& partial_freqs,
                          double floor_db = -60.0);

/// 简化 BS.1770 响度 (LUFS): K 加权 + 400ms 门限
double measure_lufs(const std::vector<float>& mono, int sample_rate);

// ── 主渲染入口 ────────────────────────────────────────────────────
std::vector<float> render_document(const RcpDocument& doc,
                                   const RenderOptions& opt = {},
                                   RenderStats* stats = nullptr);

std::vector<float> render_rcp_unified(std::string_view content,
                                      const std::vector<double>& fallback_harmonics,
                                      int sample_rate = 44100,
                                      std::vector<HarmonicSustain>* sustain_out = nullptr,
                                      size_t* note_count = nullptr);

bool render_rcp_to_wav(std::string_view content,
                       const std::vector<double>& fallback_harmonics,
                       std::string_view out_path,
                       const RenderOptions& opt = {});

// ── 底层单音合成 (供测试/分析) ────────────────────────────────────
/**
 * 生成一个音符的波形采样数据。
 *
 *  - 发声长度 = 记谱时值(gate) + 制音器衰减 / 自然余音 (不再硬截断)
 *  - 分音由 build_partials 给出 (低音区分音多, 高音区分音少)
 *  - 包络: 起音(力度相关) + 单/双指数衰减 + 制音器 + 末尾最小淡出
 *  - 噪声: 用同一组分音整形 (模态噪声), 而非未滤波白噪
 *
 * @param frequency     基频 (Hz); <=0 表示休止符 (返回全零)
 * @param duration_sec  记谱时值/按键时长 (秒)
 * @param harmonics     显式谐波振幅表 (文档给出时优先); 空则由 acoustic 推导
 * @param dbg           可选: 输出分音/时长诊断
 */
std::vector<float> generate_tone(double frequency,
                                 double duration_sec,
                                 const std::vector<double>& harmonics,
                                 int sample_rate = 44100,
                                 const AcousticParams& acoustic = {},
                                 const NoteOverrides& ov = {},
                                 double velocity = 1.0,
                                 uint32_t seed = 0);

/// 由物理参数推导各分音初始振幅 (旧接口: br^(n-1) 律, br=1 时平坦)
std::vector<double> harmonics_from_acoustic(const AcousticParams& p, int count = 12);

/// 第 n 个分音的频率 f_n = n·f1·sqrt(1 + B·n²)
double partial_frequency(double f1, int n, double inharmonicity);

/// @deprecated 旧接口: 追加静音间隔 (新渲染不再插入硬间隔)
void add_gap(std::vector<float>& samples,
             double gap_sec = 0.05,
             int sample_rate = 44100);

#endif // MUSIC_TONE_GEN_H
