#ifndef MUSIC_NOTE_PARSER_H
#define MUSIC_NOTE_PARSER_H

#include <string>
#include <vector>
#include <string_view>
#include <unordered_map>

/// 解析后的单个音符
struct Note {
    double frequency;    ///< 频率 (Hz)
    double duration_sec; ///< 持续时间 (秒)
};

/**
 * 简谱字符串解析器
 *
 * 音符格式: <音>.<八度拍长>
 *   音: 1~7 (唱名), 0 (休止符)
 *   八度: 0=中音, -=低八度, +=高八度, 可连续 (++=高两个八度, --=低两个八度)
 *   拍长分母: 4=四分音符, 2=二分音符, 8=八分音符, 1=全音符, :代替.表示连音
 *
 * 例: "3.+2" → 3 的高八度二分音符
 */
class NoteParser {
public:
    explicit NoteParser(double base_freq = 261.63,
                        double base_beat_duration = 0.5);

    /// 解析单个音符字符串
    [[nodiscard]] Note parse(std::string_view note_str) const;

    /// 通过 BPM 设置标准拍长 (base_beat_duration = 60 / bpm)
    void set_bpm(double bpm);

    // Setters
    void set_base_freq(double freq) { base_freq_ = freq; }
    void set_base_beat_duration(double dur) { base_beat_duration_ = dur; }

    // Getters
    double base_freq() const { return base_freq_; }
    double base_beat_duration() const { return base_beat_duration_; }

private:
    double base_freq_;           ///< 基准频率 (C4 = 261.63Hz)
    double base_beat_duration_;  ///< 四分音符时长 (秒)

    static const std::unordered_map<char, int> NOTE_TO_SEMITONE;
};

// ── RCP 文件解析辅助函数 ──────────────────────────────────────────

/// 解析 RCP 文件第一行: "BPM,base_freq,base_beat_duration"
/// 返回 true 成功; 三个输出参数按顺序填入
bool parse_rcp_header(std::string_view line,
                      double& bpm,
                      double& base_freq,
                      double& base_beat_duration);

/// 将 RCP 文件内容 (去头后) 分割成单个音符字符串列表
std::vector<std::string> tokenize_notes(const std::vector<std::string>& lines);

#endif // MUSIC_NOTE_PARSER_H
