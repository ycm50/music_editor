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

/**
 * 生成一个音符的波形采样数据
 *
 * @param frequency     基频 (Hz)
 * @param duration_sec  时长 (秒)
 * @param harmonics     泛音振幅系数 (第 i 个元素是第 i+1 次谐波)
 * @param sample_rate   采样率 (Hz)
 * @return float32 采样数组, 值域 [-1.0, 1.0]
 */
std::vector<float> generate_tone(double frequency,
                                  double duration_sec,
                                  const std::vector<double>& harmonics,
                                  int sample_rate = 44100);

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
