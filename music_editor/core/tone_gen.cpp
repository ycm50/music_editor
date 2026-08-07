#include "tone_gen.h"
#include "note_parser.h"

#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <numbers>
#include <sstream>
#include <stdexcept>

// ── 音色查找 ───────────────────────────────────────────────────────
static std::string to_lower(std::string_view s)
{
    std::string r;
    r.reserve(s.size());
    for (char c : s) r.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return r;
}

const Timbre* Timbres::find_by_name(std::string_view name)
{
    auto lower = to_lower(name);
    for (const auto& t : all()) {
        if (to_lower(t.name) == lower)
            return &t;
    }
    return nullptr;
}

const std::vector<Timbre>& Timbres::all()
{
    static const std::vector<Timbre> instances = {PIANO, VIOLIN, FLUTE};
    return instances;
}

// ── 波形生成 ───────────────────────────────────────────────────────
namespace {
// 单个谐波在 t (0~1) 时刻的振幅倍率
double sustain_gain(const std::vector<HarmonicSustain>& sustain,
                    size_t idx, double t)
{
    if (sustain.empty() || idx >= sustain.size())
        return 1.0;
    const auto& regions = sustain[idx].regions;
    if (regions.empty())
        return 1.0;
    for (const auto& r : regions) {
        // 零长度区间 (0-0): 该倍频在所有时间不存在
        if (r.start == r.end)
            continue;
        if (t >= r.start && t <= r.end) {
            if (!r.fade_out)
                return 1.0;
            double span = r.end - r.start;
            if (span <= 0.0)
                return 1.0;
            return std::clamp((r.end - t) / span, 0.0, 1.0);
        }
    }
    return 0.0;
}
}

std::vector<float> generate_tone(double frequency,
                                  double duration_sec,
                                  const std::vector<double>& harmonics,
                                  int sample_rate,
                                  const std::vector<HarmonicSustain>& sustain)
{
    int num_samples = static_cast<int>(sample_rate * duration_sec);
    if (num_samples <= 0) return {};

    std::vector<float> tone(num_samples, 0.0f);

    // 合成各次谐波 (按持续比例做时间包络)
    for (size_t i = 0; i < harmonics.size(); ++i) {
        double amp = harmonics[i];
        if (amp == 0.0) continue;

        double harmonic_freq = frequency * static_cast<double>(i + 1);
        double phase_inc = 2.0 * std::numbers::pi * harmonic_freq / sample_rate;

        for (int s = 0; s < num_samples; ++s) {
            double t = static_cast<double>(s) / num_samples;
            double g = sustain_gain(sustain, i, t);
            if (g <= 0.0) continue;
            tone[s] += static_cast<float>(amp * g * std::sin(phase_inc * s));
        }
    }

    // 归一化到 [-1.0, 1.0]
    float max_val = 0.0f;
    for (float v : tone) {
        float abs_v = std::abs(v);
        if (abs_v > max_val) max_val = abs_v;
    }
    if (max_val > 0.0f) {
        for (float& v : tone) v /= max_val;
    }

    // 淡入淡出 (50ms) 减少爆音
    int fade_samples = static_cast<int>(sample_rate * 50 / 1000);
    fade_samples = std::min(fade_samples, num_samples / 2);

    if (fade_samples > 0) {
        // 淡入
        for (int i = 0; i < fade_samples; ++i) {
            tone[i] *= static_cast<float>(i) / static_cast<float>(fade_samples);
        }
        // 淡出
        for (int i = 0; i < fade_samples; ++i) {
            int idx = num_samples - 1 - i;
            tone[idx] *= static_cast<float>(i) / static_cast<float>(fade_samples);
        }
    }

    return tone;
}

// ── 间隔静音 ───────────────────────────────────────────────────────
void add_gap(std::vector<float>& samples, double gap_sec, int sample_rate)
{
    int gap_samples = static_cast<int>(sample_rate * gap_sec);
    if (gap_samples <= 0) return;
    samples.resize(samples.size() + gap_samples, 0.0f);
}

// ── 持续比例行解析 ─────────────────────────────────────────────────
namespace {
std::string trim_ws(std::string_view s)
{
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return {};
    auto e = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(b, e - b + 1));
}

// 解析单个区间 "0.1-0.3" 或 "0.1-0.3>" (0<=start<end<=1)
SustainRegion parse_region(std::string_view tok)
{
    bool fade = false;
    if (!tok.empty() && tok.back() == '>') {
        fade = true;
        tok = tok.substr(0, tok.size() - 1);
    }

    auto dash = tok.find('-');
    if (dash == std::string_view::npos)
        throw std::invalid_argument("sustain region missing '-': " + std::string(tok));

    auto parse_num = [](std::string_view sv) {
        std::string s = trim_ws(sv);
        char* end = nullptr;
        double v = std::strtod(s.c_str(), &end);
        if (s.empty() || *end != '\0')
            throw std::invalid_argument("invalid sustain number: " + s);
        return v;
    };

    double start = parse_num(tok.substr(0, dash));
    double end   = parse_num(tok.substr(dash + 1));
    // 0 <= start <= end <= 1; start == end (如 0-0) 表示该倍频在所有时间都不存在
    if (!(start >= 0.0 && end <= 1.0 && start <= end))
        throw std::invalid_argument("sustain region out of range [0,1]: " + std::string(tok));

    return {start, end, fade};
}
}

std::vector<HarmonicSustain> parse_sustain_line(std::string_view line)
{
    std::vector<HarmonicSustain> result;

    // 按 '!' 分割各项 (每项对应一个倍频)
    std::stringstream ss{std::string(line)};
    std::string token;
    while (std::getline(ss, token, '!')) {
        std::string t = trim_ws(token);
        if (t.empty()) continue;

        HarmonicSustain hs;
        if (t.front() == '{') {
            if (t.back() != '}')
                throw std::invalid_argument("unbalanced '{' in sustain: " + t);
            std::string inner = t.substr(1, t.size() - 2);
            std::stringstream is(inner);
            std::string region;
            while (std::getline(is, region, ',')) {
                if (trim_ws(region).empty()) continue;
                hs.regions.push_back(parse_region(trim_ws(region)));
            }
            if (hs.regions.empty())
                throw std::invalid_argument("empty sustain group: " + t);
        } else {
            hs.regions.push_back(parse_region(t));
        }
        result.push_back(std::move(hs));
    }
    return result;
}

// ── 统一 RCP 格式解析 (手机端/桌面端共用) ───────────────────────────
bool is_harmonics_line(std::string_view line)
{
    if (line.empty()) return false;
    std::stringstream ss{std::string(line)};
    std::string tok;
    bool any = false;
    while (std::getline(ss, tok, ',')) {
        std::string t = trim_ws(tok);
        if (t.empty()) return false;
        char* end = nullptr;
        double v = std::strtod(t.c_str(), &end);
        if (*end != '\0' || v < 0.0) return false;
        any = true;
    }
    return any;
}

bool is_sustain_line(std::string_view line)
{
    try {
        return !parse_sustain_line(line).empty();
    } catch (const std::exception&) {
        return false;
    }
}

static std::vector<double> parse_harmonics_line(std::string_view line)
{
    std::vector<double> h;
    std::stringstream ss{std::string(line)};
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        std::string t = trim_ws(tok);
        if (t.empty()) continue;
        h.push_back(std::strtod(t.c_str(), nullptr));
    }
    return h;
}

std::vector<float> render_rcp_unified(std::string_view content,
                                      const std::vector<double>& fallback_harmonics,
                                      int sample_rate,
                                      std::vector<HarmonicSustain>* sustain_out,
                                      size_t* note_count)
{
    // 去除 UTF-8 BOM
    size_t start = 0;
    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        start = 3;
    }

    std::stringstream ss{std::string(content.substr(start))};
    std::string first_line;
    std::getline(ss, first_line);

    double bpm, base_freq, base_beat_dur;
    if (!parse_rcp_header(first_line, bpm, base_freq, base_beat_dur))
        throw std::runtime_error("Invalid RCP header: " + first_line);

    // 剩余行: 第2行可为音色, 第3行可为持续比例, 其后为音符
    std::vector<std::string> lines;
    {
        std::string line;
        while (std::getline(ss, line)) {
            std::string t = trim_ws(line);
            if (!t.empty()) lines.push_back(t);
        }
    }

    NoteParser parser(base_freq, base_beat_dur);
    std::vector<double> harmonics = fallback_harmonics;
    std::vector<HarmonicSustain> sustain;

    size_t idx = 0;
    // 第 2 行: 可选音色行
    if (idx < lines.size() && is_harmonics_line(lines[idx])) {
        harmonics = parse_harmonics_line(lines[idx]);
        ++idx;
        // 第 3 行: 可选持续比例行
        if (idx < lines.size() && is_sustain_line(lines[idx])) {
            sustain = parse_sustain_line(lines[idx]);
            ++idx;
        }
    }

    // 其余行为音符
    std::vector<float> audio;
    size_t notes = 0;
    for (; idx < lines.size(); ++idx) {
        std::stringstream ls(lines[idx]);
        std::string token;
        while (ls >> token) {
            if (token.empty()) continue;
            ++notes;
            auto note = parser.parse(token);
            auto tone = generate_tone(note.frequency, note.duration_sec,
                                      harmonics, sample_rate, sustain);
            audio.insert(audio.end(), tone.begin(), tone.end());
            add_gap(audio, 0.05, sample_rate);
        }
    }

    if (sustain_out) *sustain_out = std::move(sustain);
    if (note_count) *note_count = notes;
    return audio;
}
