#include "note_parser.h"

#include <cmath>
#include <cctype>
#include <sstream>
#include <stdexcept>

// ── 音名 → 半音偏移 ──────────────────────────────────────────────
const std::unordered_map<char, int> NoteParser::NOTE_TO_SEMITONE = {
    {'1', 0}, {'2', 2}, {'3', 4}, {'4', 5},
    {'5', 7}, {'6', 9}, {'7', 11}, {'0', -240},
};

// ── 构造 ───────────────────────────────────────────────────────────
NoteParser::NoteParser(double base_freq, double base_beat_duration)
    : base_freq_(base_freq)
    , base_beat_duration_(base_beat_duration)
{}

void NoteParser::set_bpm(double bpm)
{
    if (bpm <= 0.0)
        throw std::invalid_argument("BPM must be positive");
    base_beat_duration_ = 60.0 / bpm;
}

// ── 解析单音符 ─────────────────────────────────────────────────────
Note NoteParser::parse(std::string_view note_str) const
{
    // 按 '.' 分割
    auto dot_pos = note_str.find('.');
    if (dot_pos == std::string_view::npos)
        throw std::invalid_argument(
            "Invalid note format (missing '.'): " + std::string(note_str));

    char note_sym = note_str[0];
    std::string_view suffix = note_str.substr(dot_pos + 1);

    auto it = NOTE_TO_SEMITONE.find(note_sym);
    if (it == NOTE_TO_SEMITONE.end())
        throw std::invalid_argument(
            "Invalid note symbol: " + std::string(1, note_sym));

    // 解析八度偏移 (必须显式写: 0=中音, +=高八度, -=低八度, 省略无效)
    int octave_offset = 0;
    std::string_view beat_str;

    if (suffix.empty())
        throw std::invalid_argument(
            "Invalid note format (missing octave marker): " + std::string(note_str));

    if (suffix[0] == '0') {
        // 中音
        octave_offset = 0;
        beat_str = suffix.substr(1);
    } else {
        // 低/高八度: 支持连续标记 (++ = 高两个八度, -- = 低两个八度)
        size_t i = 0;
        while (i < suffix.size() && (suffix[i] == '+' || suffix[i] == '-')) {
            octave_offset += (suffix[i] == '+') ? 1 : -1;
            ++i;
        }
        if (i == 0)
            throw std::invalid_argument(
                "Invalid octave marker (must be 0/+/-): " + std::string(note_str));
        beat_str = suffix.substr(i);
    }

    // 把 ':' 换回 '.' (连音表示)
    std::string beat_str_fixed;
    beat_str_fixed.reserve(beat_str.size());
    for (char c : beat_str) {
        beat_str_fixed.push_back(c == ':' ? '.' : c);
    }

    if (beat_str_fixed.empty())
        throw std::invalid_argument(
            "Missing beat denominator in: " + std::string(note_str));

    // 解析拍长分母
    char* end = nullptr;
    double beat_denom = std::strtod(beat_str_fixed.c_str(), &end);
    if (*end != '\0' || beat_denom <= 0.0)
        throw std::invalid_argument(
            "Invalid beat denominator in: " + std::string(note_str));

    // 计算频率
    int semitone_shift = octave_offset * 12 + it->second;
    double freq = base_freq_ * std::pow(2.0, semitone_shift / 12.0);

    // 计算持续时间 (四分音符 = base_beat_duration_)
    double duration = base_beat_duration_ * (4.0 / beat_denom);

    return {freq, duration};
}

// ── RCP 头解析 ─────────────────────────────────────────────────────
bool parse_rcp_header(std::string_view line,
                      double& bpm,
                      double& base_freq,
                      double& base_beat_duration)
{
    // 格式: "BPM,base_freq,base_beat_duration"
    std::string s(line);
    std::vector<double> vals;
    std::stringstream ss(s);
    std::string token;

    while (std::getline(ss, token, ',')) {
        // 去除 token 首尾空白 (含 \r, 兼容 CRLF 行尾)
        auto first = token.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return false;
        auto last = token.find_last_not_of(" \t\r\n");
        token = token.substr(first, last - first + 1);

        char* end = nullptr;
        double v = std::strtod(token.c_str(), &end);
        if (*end != '\0')
            return false;
        vals.push_back(v);
    }

    if (vals.size() != 3)
        return false;

    bpm = vals[0];
    base_freq = vals[1];
    base_beat_duration = vals[2];
    return true;
}

// ── RCP 音符分词 ─────────────────────────────────────────────────
std::vector<std::string> tokenize_notes(const std::vector<std::string>& lines)
{
    std::vector<std::string> notes;
    for (const auto& line : lines) {
        std::stringstream ss(line);
        std::string tok;
        while (ss >> tok) {
            if (!tok.empty())
                notes.push_back(std::move(tok));
        }
    }
    return notes;
}
