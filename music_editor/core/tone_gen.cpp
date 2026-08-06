#include "tone_gen.h"

#include <cmath>
#include <algorithm>
#include <cctype>
#include <numbers>

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
std::vector<float> generate_tone(double frequency,
                                  double duration_sec,
                                  const std::vector<double>& harmonics,
                                  int sample_rate)
{
    int num_samples = static_cast<int>(sample_rate * duration_sec);
    if (num_samples <= 0) return {};

    std::vector<float> tone(num_samples, 0.0f);

    // 合成各次谐波
    for (size_t i = 0; i < harmonics.size(); ++i) {
        double amp = harmonics[i];
        if (amp == 0.0) continue;

        double harmonic_freq = frequency * static_cast<double>(i + 1);
        double phase_inc = 2.0 * std::numbers::pi * harmonic_freq / sample_rate;

        for (int s = 0; s < num_samples; ++s) {
            tone[s] += static_cast<float>(amp * std::sin(phase_inc * s));
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
