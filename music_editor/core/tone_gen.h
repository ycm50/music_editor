#ifndef MUSIC_TONE_GEN_H
#define MUSIC_TONE_GEN_H

#include "note_parser.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// ── 内置音色 (旧接口: 裸谐波振幅表) ───────────────────────────────
/// 音色描述
struct Timbre {
    std::string name;
    std::vector<double> harmonics; ///< 各次谐波振幅系数 [基频, 2次, 3次, ...]
};

/// 预定义音色库（UI 与 save 共享同一份数据，消除不一致）
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

    /// 按名称查找音色 (不区分大小写), 返回 nullptr 表示未找到
    const Timbre* find_by_name(std::string_view name);

    /// 所有可用音色列表
    const std::vector<Timbre>& all();
}

// ── 渲染选项 ──────────────────────────────────────────────────────
struct RenderOptions {
    int  sample_rate      = 44100;
    bool stereo           = false;  ///< true = 输出立体声 (左右交错)
    double reverb_wet     = 0.28;   ///< 混响湿量 0~1 (0 = 关闭)
    double headroom       = 0.5;    ///< 混音余量 (单音峰值上限)
    bool global_normalize = true;   ///< 整曲一次性峰值归一 (保留相对力度)
    bool dur_compensate   = false;  ///< 短音能量补偿 (等响, 抗"短音偏轻")
    bool hold_last_note   = true;   ///< 末音不被总时长截断
};

// ── 渲染结果与体检指标 ────────────────────────────────────────────
/// 单个发声事件的物理参数报告 (供 --analyze 输出)
struct NotePhysics {
    int    index         = 0;
    double frequency     = 0.0;   ///< 基频 (Hz)
    double duration_sec  = 0.0;
    double velocity      = 1.0;
    bool   rest          = false;
    int    partials      = 0;     ///< 参与合成的分音数
    int    discarded     = 0;     ///< 越过 Nyquist 被丢弃的分音数 (抗混叠)
    double centroid_hz   = 0.0;   ///< 频谱重心, Σ A_n·f_n / Σ A_n
    double tau1_sec      = 0.0;   ///< 第 1 分音衰减时间常数
    double tonic_cents   = 0.0;   ///< 相对基准频率的音分偏差 (12-TET 校验)
};

/// 整曲体检报告 (--analyze)
struct RenderStats {
    double duration_sec   = 0.0;
    size_t total_samples  = 0;    ///< 单声道样本数
    double peak           = 0.0;
    double rms            = 0.0;
    double dc_offset      = 0.0;  ///< 直流偏置 (真静音应约 0)
    double crest_db       = 0.0;  ///< 峰均比 (dB)
    double silence_ratio  = 0.0;  ///< |x|<1e-4 的样本占比
    double clipped_ratio  = 0.0;  ///< |x|>=0.999 的样本占比
    int    note_count     = 0;
    int    rest_count     = 0;
    int    chords         = 0;    ///< 同时发声 >1 的音块数
    int    max_polyphony  = 0;
    int    discarded_total = 0;   ///< 整曲被丢弃 (抗混叠) 的分音总数
    double bar_sec        = 0.0;  ///< 拍号对应的小节时长 (0 = 未声明)
    double bars           = 0.0;  ///< 总小节数 (估算)
    double bar_fit_error  = 0.0;  ///< 音符总长对不齐小节的偏差 (秒)
    double freq_min       = 0.0;
    double freq_max       = 0.0;
    std::string instrument;       ///< 物理模型名
    std::vector<std::string> warnings;
    std::vector<NotePhysics> notes;
};

// ── 主渲染入口 ────────────────────────────────────────────────────
/**
 * 把 RcpDocument 渲染成音频
 *
 * 单声道时返回 N 个样本; 立体声时返回 2N 个交错样本 (L,R,L,R...)。
 * 值域 [-1,1], 最后按 global_normalize 统一缩放 (保留相对力度)。
 *
 * @param stats 可为空; 非空时回填体检指标
 */
std::vector<float> render_document(const RcpDocument& doc,
                                   const RenderOptions& opt = {},
                                   RenderStats* stats = nullptr);

/**
 * 兼容入口: 直接渲染统一 RCP 内容 (手机端/桌面端共用)
 *
 * @param content            RCP 完整内容
 * @param fallback_harmonics 文件未提供音色行时使用的谐波振幅
 * @param sample_rate       采样率 (Hz)
 * @param sustain_out       输出解析到的旧式持续比例 (可为空)
 * @param note_count        输出发声事件个数 (可为空)
 * @return float32 采样 (单声道), 值域 [-1,1]
 */
std::vector<float> render_rcp_unified(std::string_view content,
                                      const std::vector<double>& fallback_harmonics,
                                      int sample_rate = 44100,
                                      std::vector<HarmonicSustain>* sustain_out = nullptr,
                                      size_t* note_count = nullptr);

/// 渲染并把结果写入 WAV (自动按需立体声)
bool render_rcp_to_wav(std::string_view content,
                       const std::vector<double>& fallback_harmonics,
                       std::string_view out_path,
                       const RenderOptions& opt = {});

// ── 底层单音合成 (供测试/分析) ────────────────────────────────────
/**
 * 生成一个音符的波形采样数据
 *
 * 包络基于**绝对时间**: A_n(t) = A_n(0)·(sus + (1-sus)·e^(-t/τ_n))
 * 与音符时长解耦 (弹 1 拍与弹 4 拍, 同一时刻的振幅完全相同)。
 * 超过 Nyquist 的分音被丢弃 (抗混叠)。
 *
 * @param frequency     基频 (Hz); <=0 表示休止符 (返回全零)
 * @param duration_sec  时长 (秒)
 * @param harmonics     泛音振幅系数 (第 i 个元素是第 i+1 次谐波); 空则由 acoustic 推导
 * @param acoustic      物理模型
 * @param ov            逐音覆盖
 * @param velocity      力度 (0~1)
 * @return float32 采样数组, 值域 [-1, 1], 不做整体归一化
 */
std::vector<float> generate_tone(double frequency,
                                 double duration_sec,
                                 const std::vector<double>& harmonics,
                                 int sample_rate = 44100,
                                 const AcousticParams& acoustic = {},
                                 const NoteOverrides& ov = {},
                                 double velocity = 1.0);

/// 由物理参数推导各分音初始振幅 (显式谐波表为空时使用)
std::vector<double> harmonics_from_acoustic(const AcousticParams& p, int count = 12);

/// 第 n 个分音的频率 f_n = n·f1·sqrt(1 + B·n²)
double partial_frequency(double f1, int n, double inharmonicity);

/// @deprecated 旧接口: 追加静音间隔 (新渲染不再插入硬间隔)
void add_gap(std::vector<float>& samples,
             double gap_sec = 0.05,
             int sample_rate = 44100);

#endif // MUSIC_TONE_GEN_H
