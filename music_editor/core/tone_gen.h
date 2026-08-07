#ifndef MUSIC_TONE_GEN_H
#define MUSIC_TONE_GEN_H

#include <vector>
#include <string>
#include <string_view>

/// 音色描述
struct Timbre {
    std::string name;
    std::vector<double> harmonics; ///< 各次谐波振幅系数 [基频, 2次, 3次, ...]
};

/// 预定义音色库（UI 与 save 共享同一份数据，消除不一致）
namespace Timbres {
    /// 钢琴音色 (10 次谐波)
    inline const Timbre PIANO{
        "piano",
        {1.0, 0.7, 0.5, 0.3, 0.2, 0.15, 0.1, 0.08, 0.05, 0.03}
    };

    /// 小提琴音色 (7 次谐波)
    inline const Timbre VIOLIN{
        "violin",
        {1.0, 0.7, 0.5, 0.3, 0.2, 0.15, 0.1}
    };

    /// 长笛音色 (4 次谐波)
    inline const Timbre FLUTE{
        "flute",
        {1.0, 0.2, 0.1, 0.05}
    };

    /// 按名称查找音色 (不区分大小写), 返回 nullptr 表示未找到
    const Timbre* find_by_name(std::string_view name);

    /// 所有可用音色列表
    const std::vector<Timbre>& all();
}

/// 单个谐波的持续区间 (相对音符总时长, 0~1)
struct SustainRegion {
    double start;       ///< 区间起点 (0~1)
    double end;         ///< 区间终点 (0~1)
    bool fade_out;      ///< '>' 标记: 区间内从基准振幅线性降到 0
};

/// 一个谐波的持续比例; regions 为空表示全程恒有 (0-1)
struct HarmonicSustain {
    std::vector<SustainRegion> regions;
};

/**
 * 解析持续比例行 (RCP 第三行, 各项用 '!' 分隔, 每项对应第二行的一个谐波)
 *
 * 三种形式:
 *   "0-1"                全程恒有 (第二行振幅)
 *   "0-0"                该倍频在所有时间不存在 (相当于静音)
 *   "0.3-0.5"            在 0.3~0.5 区间稳定为第二行振幅
 *   "0.1-0.3>"           在 0.1~0.3 从第二行振幅线性降到 0
 *   "{0.1-0.3,0.5-0.7}"  多个区间 (可混用 > 标记 / 0-0)
 * 例: "0-1!0.3-0.5!{0.1-0.3,0.5-0.7}"
 *
 * @throws std::invalid_argument 格式错误或区间越界
 */
std::vector<HarmonicSustain> parse_sustain_line(std::string_view line);

/// 判断一行是否为音色行 (逗号分隔的非负数字), 用于识别统一格式第 2 行
bool is_harmonics_line(std::string_view line);

/// 判断一行是否形似持续比例行 (可被 parse_sustain_line 解析), 用于识别统一格式第 3 行
bool is_sustain_line(std::string_view line);

/**
 * 渲染统一 RCP 内容 (手机端/桌面端共用格式):
 *   第 1 行: BPM,基准频率,标准拍长
 *   第 2 行(可选): 音色, 逗号分隔谐波振幅
 *   第 3 行(可选): 持续比例, '!' 分隔各谐波区间
 *   其余行: 音符
 *
 * @param content      RCP 完整内容
 * @param fallback_harmonics 文件未提供音色行时使用的谐波
 * @param sample_rate  采样率 (Hz)
 * @param sustain_out  输出解析到的持续比例 (可为空)
 * @param note_count   输出音符个数 (可为空)
 * @return float32 采样数组, 值域 [-1.0, 1.0]
 */
std::vector<float> render_rcp_unified(std::string_view content,
                                      const std::vector<double>& fallback_harmonics,
                                      int sample_rate = 44100,
                                      std::vector<HarmonicSustain>* sustain_out = nullptr,
                                      size_t* note_count = nullptr);

/**
 * 生成一个音符的波形采样数据
 *
 * @param frequency     基频 (Hz)
 * @param duration_sec  时长 (秒)
 * @param harmonics     泛音振幅系数 (第 i 个元素是第 i+1 次谐波)
 * @param sample_rate   采样率 (Hz)
 * @param sustain       各谐波持续比例 (为空则全部全程恒有)
 * @return float32 采样数组, 值域 [-1.0, 1.0]
 */
std::vector<float> generate_tone(double frequency,
                                  double duration_sec,
                                  const std::vector<double>& harmonics,
                                  int sample_rate = 44100,
                                  const std::vector<HarmonicSustain>& sustain = {});

/**
 * 添加音符间隔静音 (默认 50ms)
 *
 * @param samples       原采样数组 (会被追加)
 * @param gap_sec       间隔时长 (秒)
 * @param sample_rate   采样率
 */
void add_gap(std::vector<float>& samples,
             double gap_sec = 0.05,
             int sample_rate = 44100);

#endif // MUSIC_TONE_GEN_H
