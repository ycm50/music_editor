#include "tone_gen.h"
#include "wav_writer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <sstream>
#include <stdexcept>

namespace {

constexpr double kPi = std::numbers::pi;

std::string to_lower(std::string_view s)
{
    std::string r;
    r.reserve(s.size());
    for (char c : s)
        r.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return r;
}

// ── 确定性伪随机 (保证同一份谱每次渲染结果一致) ──────────────────
inline uint32_t hash_u32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
inline double hash_unit(uint32_t seed)   // [0,1)
{
    return hash_u32(seed) * (1.0 / 4294967296.0);
}

/// 声部槽: 记录 [start,end) 区间, 用于统计最大同时发声数
struct Slot { double start = 0.0; double end = -1.0; };

} // namespace

// ── 音色查找 ───────────────────────────────────────────────────────
const Timbre* Timbres::find_by_name(std::string_view name)
{
    auto lower = to_lower(name);
    for (const auto& t : all())
        if (to_lower(t.name) == lower)
            return &t;
    return nullptr;
}

const std::vector<Timbre>& Timbres::all()
{
    static const std::vector<Timbre> instances = {PIANO, VIOLIN, FLUTE};
    return instances;
}

// ── 物理量 ─────────────────────────────────────────────────────────
double partial_frequency(double f1, int n, double inharmonicity)
{
    if (inharmonicity <= 0.0)
        return f1 * n;
    return f1 * n * std::sqrt(1.0 + inharmonicity * static_cast<double>(n) * n);
}

std::vector<double> harmonics_from_acoustic(const AcousticParams& p, int count)
{
    count = std::clamp(count, 1, 64);
    std::vector<double> h(static_cast<size_t>(count));
    for (int n = 1; n <= count; ++n)
        h[static_cast<size_t>(n - 1)] = std::pow(p.brightness, n - 1);
    return h;
}

// ── 单音合成 ───────────────────────────────────────────────────────
std::vector<float> generate_tone(double frequency,
                                 double duration_sec,
                                 const std::vector<double>& harmonics,
                                 int sample_rate,
                                 const AcousticParams& acoustic,
                                 const NoteOverrides& ov,
                                 double velocity)
{
    if (duration_sec <= 0.0 || sample_rate <= 0) return {};
    const int num = static_cast<int>(sample_rate * duration_sec);
    if (num <= 0) return {};

    // 休止符 / 无基频 → 真静音 (不再合成 0.25mHz 正弦)
    if (frequency <= 0.0)
        return std::vector<float>(static_cast<size_t>(num), 0.0f);

    AcousticParams a = acoustic;
    if (ov.has_atk) a.attack        = ov.atk;
    if (ov.has_dec) a.decay         = ov.dec;
    if (ov.has_sus) a.sustain       = ov.sus;
    if (ov.has_rel) a.release       = ov.rel;
    if (ov.has_br)  a.brightness    = ov.br;
    if (ov.has_B)   a.inharmonicity = ov.B;

    std::vector<double> amps = harmonics.empty()
                                   ? harmonics_from_acoustic(a)
                                   : harmonics;

    const double sr  = sample_rate;
    const double nyq = sr * 0.5;
    const int    np  = static_cast<int>(amps.size());
    const int    atk_n = std::clamp(static_cast<int>(a.attack * sr), 1,
                                    std::max(1, num / 2));

    // 幅度标定: 用分音平方和的平方根做音频域标定, 峰值不超过 headroom
    double sumsq = 0.0;
    for (double x : amps) sumsq += x * x;
    const double norm = 1.0 / std::sqrt(std::max(sumsq, 1e-12));

    const double vel = std::clamp(velocity, 0.0, 1.0);

    std::vector<float> out(static_cast<size_t>(num), 0.0f);

    for (int k = 0; k < np; ++k) {
        const double amp0 = amps[static_cast<size_t>(k)];
        if (amp0 <= 0.0) continue;
        const int n = k + 1;
        const double f = partial_frequency(frequency, n, a.inharmonicity);
        if (f >= nyq) continue;                        // 抗混叠: 丢弃越界分音
        const double w = 2.0 * kPi * f / sr;

        // 逐分音衰减时间常数 τ_n
        double tau;
        if (!a.partial_decay.empty() && static_cast<size_t>(k) < a.partial_decay.size())
            tau = a.partial_decay[static_cast<size_t>(k)];
        else
            tau = a.decay / std::pow(static_cast<double>(n), a.damping_exp);
        if (!(tau > 0.0)) tau = a.decay;

        // 确定性的初相位: 让瞬间起音的能量更接近噪声而非同相冲激
        const double phi = hash_unit(0x9e3779b9u ^ static_cast<uint32_t>(
                                        static_cast<int>(frequency * 100.0) * 131 + n)) * 2.0 * kPi;

        const double inv_tau = 1.0 / tau;
        const double A0 = amp0 * vel * norm;

        for (int s = 0; s < num; ++s) {
            const double t = static_cast<double>(s) / sr;
            // 包络: 起音 (平方上升) → 稳态/指数衰减 (绝对时间)
            double env;
            if (s < atk_n) {
                const double p = static_cast<double>(s) / static_cast<double>(atk_n);
                env = p * p;
            } else {
                const double td = t - a.attack;
                const double dec = (td > 0.0) ? std::exp(-td * inv_tau) : 1.0;
                env = a.sustain + (1.0 - a.sustain) * dec;
            }
            out[static_cast<size_t>(s)] += static_cast<float>(
                A0 * env * std::sin(w * static_cast<double>(s) + phi));
        }
    }

    // 起音噪声 (弓毛/气流/击弦噪声): 短促噪声, 提升真实感
    if (a.noise > 0.0) {
        const int noise_n = std::min(num, static_cast<int>(0.012 * sr));
        for (int s = 0; s < noise_n; ++s) {
            const double p = static_cast<double>(s) / static_cast<double>(noise_n);
            const double envn = (1.0 - p) * (1.0 - p) * a.noise * vel * 0.35;
            const double r = hash_unit(0x51ed2701u + static_cast<uint32_t>(s)
                                                      + static_cast<uint32_t>(frequency)) * 2.0 - 1.0;
            out[static_cast<size_t>(s)] += static_cast<float>(envn * r);
        }
    }

    // 谱面结束后的自然收尾 (release): cos² 衰落, 取代旧的硬编码 50ms 线性淡出
    if (a.release > 0.0) {
        const int rel_n = std::min(num, static_cast<int>(a.release * sr));
        for (int s = 0; s < rel_n; ++s) {
            const double p = static_cast<double>(s) / static_cast<double>(rel_n);
            const double w = 0.5 - 0.5 * std::cos(kPi * p);   // 0→1
            out[static_cast<size_t>(num - 1 - s)] *= static_cast<float>(w);
        }
    }
    return out;
}

// ── 旧接口 ─────────────────────────────────────────────────────────
void add_gap(std::vector<float>& samples, double gap_sec, int sample_rate)
{
    int gap_samples = static_cast<int>(sample_rate * gap_sec);
    if (gap_samples <= 0) return;
    samples.resize(samples.size() + static_cast<size_t>(gap_samples), 0.0f);
}

// ── 混响 (Schroeder: 4 梳状 + 2 全通, 左右去相关) ─────────────────
namespace {

struct Comb {
    std::vector<double> buf;
    int idx = 0;
    double store = 0.0;
    explicit Comb(int n) : buf(static_cast<size_t>(std::max(1, n)), 0.0) {}
    inline double process(double x, double fb)
    {
        double y = buf[static_cast<size_t>(idx)];
        store = y * 0.65 + store * 0.35;      // 一阶低通: 高频先衰减
        buf[static_cast<size_t>(idx)] = x + store * fb;
        if (++idx >= static_cast<int>(buf.size())) idx = 0;
        return y;
    }
};

struct Allpass {
    std::vector<double> buf;
    int idx = 0;
    explicit Allpass(int n) : buf(static_cast<size_t>(std::max(1, n)), 0.0) {}
    inline double process(double x, double g)
    {
        double y = buf[static_cast<size_t>(idx)];
        double v = x + g * y;
        buf[static_cast<size_t>(idx)] = v;
        if (++idx >= static_cast<int>(buf.size())) idx = 0;
        return y - g * v;
    }
};

/// 对单声道信号做立体声混响; 返回交错样本
std::vector<float> apply_reverb(const std::vector<float>& mono, int sr, double wet)
{
    wet = std::clamp(wet, 0.0, 1.0);
    auto ms = [sr](double v) { return static_cast<int>(v * sr / 1000.0); };

    const int base[4] = {ms(29.7), ms(37.1), ms(41.1), ms(43.7)};
    const int ap[2]   = {ms(5.0), ms(1.7)};

    Comb cl[4] = {Comb(base[0]), Comb(base[1]), Comb(base[2]), Comb(base[3])};
    Comb cr[4] = {Comb(base[0] + 23), Comb(base[1] + 23),
                  Comb(base[2] + 23), Comb(base[3] + 23)};
    Allpass al[2] = {Allpass(ap[0]), Allpass(ap[1])};
    Allpass ar[2] = {Allpass(ap[0] + 11), Allpass(ap[1] + 11)};

    const double fb = 0.78;   // 目标 RT60 约 1.2 s
    const double g  = 0.5;

    std::vector<float> out(mono.size() * 2, 0.0f);
    for (size_t i = 0; i < mono.size(); ++i) {
        const double x = mono[i];
        double l = 0.0, r = 0.0;
        for (int k = 0; k < 4; ++k) {
            l += cl[k].process(x, fb);
            r += cr[k].process(x, fb);
        }
        l *= 0.25; r *= 0.25;
        l = al[0].process(l, g); l = al[1].process(l, g);
        r = ar[0].process(r, g); r = ar[1].process(r, g);

        out[i * 2]     = static_cast<float>(x * (1.0 - wet * 0.5) + l * wet);
        out[i * 2 + 1] = static_cast<float>(x * (1.0 - wet * 0.5) + r * wet);
    }
    return out;
}

} // namespace

// ── 主渲染 ─────────────────────────────────────────────────────────
std::vector<float> render_document(const RcpDocument& doc,
                                   const RenderOptions& opt,
                                   RenderStats* stats)
{
    if (opt.sample_rate <= 0)
        throw std::invalid_argument("sample_rate must be positive");

    const int sr = opt.sample_rate;
    const double beat = doc.beat_duration > 0.0 ? doc.beat_duration : 0.5;

    AcousticParams acoustic = doc.has_acoustic ? doc.acoustic : Instruments::piano();

    // 分音振幅表: 显式谐波行优先, 否则由物理模型推导
    std::vector<double> amps = doc.has_harmonics
                                   ? doc.harmonics
                                   : harmonics_from_acoustic(acoustic);

    // ── 第一遍: 时间轴排版 (拍 → 绝对秒) ─────────────────────────
    // 以 "token / 音块" 为最小推进单位: 同一 token 内的事件共享起音时刻
    // (和弦纵向叠加); 连音已把时值相加, 因此更长的音会与后续音块重叠 ——
    // 这样多声部才能真正叠加, 而不是被串行排开。
    struct Event {
        double start = 0.0;
        double dur = 0.0;
        double freq = 0.0;
        double vel = 1.0;
        bool rest = false;
        NoteOverrides ov;
        int slot = 0;
    };
    std::vector<Event> events;
    events.reserve(doc.notes.size());

    double cursor = 0.0;
    std::vector<Slot> slots(16);
    int max_poly = 0;
    int chord_blocks = 0;

    for (size_t i = 0; i < doc.notes.size();) {
        size_t j = i + 1;
        while (j < doc.notes.size() && !doc.notes[j].token_start) ++j;

        const double block_start = cursor;
        double block_dur = 0.0;
        for (size_t k = i; k < j; ++k) {
            double d = doc.notes[k].duration_sec;
            if (doc.notes[k].articulation == Articulation::Staccato) d *= 0.5;
            block_dur = std::max(block_dur, d);
        }
        if (j - i > 1) ++chord_blocks;

        for (size_t k = i; k < j; ++k) {
            Event e;
            e.start = block_start;
            e.dur = doc.notes[k].duration_sec;
            if (doc.notes[k].articulation == Articulation::Staccato) e.dur *= 0.5;
            e.freq = doc.notes[k].frequency;
            e.vel = doc.notes[k].velocity;
            e.rest = doc.notes[k].rest;
            e.ov = doc.notes[k].ov;

            int slot = -1;
            for (int s = 0; s < static_cast<int>(slots.size()); ++s) {
                if (slots[static_cast<size_t>(s)].end <= e.start + 1e-9) { slot = s; break; }
            }
            if (slot < 0) {
                size_t best = 0;
                for (size_t s2 = 1; s2 < slots.size(); ++s2)
                    if (slots[s2].end < slots[best].end) best = s2;
                slot = static_cast<int>(best);
            }
            Slot& sl = slots[static_cast<size_t>(slot)];
            sl.start = e.start;
            sl.end = e.start + e.dur;
            e.slot = slot;

            if (!e.rest) {
                int now = 0;
                for (const auto& s2 : slots) {
                    if (s2.end <= 0.0) continue;
                    if (s2.end > e.start + 1e-9) ++now;
                }
                max_poly = std::max(max_poly, now);
            }
            events.push_back(e);
        }

        cursor = block_start + block_dur;
        i = j;
    }

    // ── 第二遍: 合成 + 叠加 ──────────────────────────────────────
    const double max_release = std::max(0.05, acoustic.release + 0.05);
    const double tail = max_release + (opt.reverb_wet > 0.0 ? 1.2 : 0.0);
    double total_sec = cursor + tail;
    if (events.empty()) total_sec = 0.0;
    if (opt.hold_last_note && !events.empty()) {
        const double last_end = events.back().start + events.back().dur;
        total_sec = std::max(total_sec, last_end + std::min(acoustic.decay * 1.5, 3.0)
                                              + max_release);
    }
    size_t total = static_cast<size_t>(total_sec * sr);
    if (total == 0) {
        if (stats) {
            *stats = RenderStats{};
            stats->warnings = doc.warnings;
        }
        return {};
    }

    std::vector<float> mix(total, 0.0f);

    for (auto& e : events) {
        if (e.rest) continue;
        auto tone = generate_tone(e.freq, e.dur, amps, sr, acoustic, e.ov, e.vel);
        if (tone.empty()) continue;
        size_t off = static_cast<size_t>(e.start * sr);
        if (off >= mix.size()) continue;
        const size_t cnt = std::min(tone.size(), mix.size() - off);
        for (size_t k = 0; k < cnt; ++k)
            mix[off + k] += tone[k];
    }

    // ── 去直流 (物理上正确: 任何直流都是 bug) ────────────────────
    double sum = 0.0;
    for (float v : mix) sum += v;
    const double dc = sum / static_cast<double>(mix.size());
    if (std::abs(dc) > 1e-9) {
        const float f = static_cast<float>(dc);
        for (float& v : mix) v -= f;
    }

    // ── 整曲一次性归一 (保留相对力度; 不再逐音归一) ──────────────
    float peak = 0.0f;
    for (float v : mix) peak = std::max(peak, std::fabs(v));
    if (opt.global_normalize && peak > 1e-6f) {
        double applied = 0.92 / static_cast<double>(peak);   // 留约 7% 余量
        if (applied > 1.0) applied = 1.0;   // 只缩不放, 不人为放大安静片段
        if (applied < 1.0) {
            const float f = static_cast<float>(applied);
            for (float& v : mix) v *= f;
        }
    }

    // ── 立体声 + 混响 ────────────────────────────────────────────
    std::vector<float> audio;
    if (opt.stereo && opt.reverb_wet > 0.0)
        audio = apply_reverb(mix, sr, opt.reverb_wet);
    else if (opt.stereo) {
        audio.resize(mix.size() * 2);
        for (size_t i = 0; i < mix.size(); ++i) {
            audio[i * 2] = mix[i];
            audio[i * 2 + 1] = mix[i];
        }
    } else {
        audio = std::move(mix);
    }

    // ── 体检指标 ─────────────────────────────────────────────────
    if (stats) {
        RenderStats st;
        st.warnings = doc.warnings;
        st.instrument = acoustic.name;
        st.duration_sec = static_cast<double>(audio.size()) / (opt.stereo ? 2.0 : 1.0) / sr;
        st.total_samples = audio.size() / (opt.stereo ? 2 : 1);
        st.chords = chord_blocks;
        st.max_polyphony = max_poly;

        double acc2 = 0.0, dcacc = 0.0;
        size_t silent = 0, clipped = 0;
        const size_t step = opt.stereo ? 2 : 1;
        for (size_t i = 0; i < audio.size(); i += step) {
            const double v = audio[i];
            acc2 += v * v; dcacc += v;
            st.peak = std::max(st.peak, std::fabs(v));
            if (std::fabs(v) < 1e-4) ++silent;
            if (std::fabs(v) >= 0.999) ++clipped;
        }
        const double nn = static_cast<double>(st.total_samples ? st.total_samples : 1);
        st.rms = std::sqrt(acc2 / nn);
        st.dc_offset = dcacc / nn;
        st.crest_db = st.rms > 1e-12 ? 20.0 * std::log10(st.peak / st.rms) : 0.0;
        st.silence_ratio = static_cast<double>(silent) / nn;
        st.clipped_ratio = static_cast<double>(clipped) / nn;

        int idx = 0;
        for (const auto& e : events) {
            NotePhysics np;
            np.index = idx++;
            np.frequency = e.freq;
            np.duration_sec = e.dur;
            np.velocity = e.vel;
            np.rest = e.rest;
            if (!e.rest) {
                if (e.freq > 0.0) {
                    st.freq_min = (st.freq_min == 0.0) ? e.freq : std::min(st.freq_min, e.freq);
                    st.freq_max = std::max(st.freq_max, e.freq);
                    np.tonic_cents = 1200.0 * std::log2(e.freq / doc.base_freq);
                }
                double nume = 0.0, deno = 0.0;
                for (size_t k = 0; k < amps.size(); ++k) {
                    const int n = static_cast<int>(k) + 1;
                    const double f = partial_frequency(e.freq, n, acoustic.inharmonicity);
                    if (f >= sr * 0.5) { ++np.discarded; continue; }
                    nume += amps[k] * f;
                    deno += amps[k];
                    ++np.partials;
                }
                np.centroid_hz = deno > 1e-12 ? nume / deno : 0.0;
                np.tau1_sec = (!acoustic.partial_decay.empty())
                                  ? acoustic.partial_decay.front()
                                  : acoustic.decay;
                st.discarded_total += np.discarded;
                ++st.note_count;
            } else {
                ++st.rest_count;
            }
            st.notes.push_back(np);
        }

        // 拍号校验: 音符总时长能否被小节整除
        if (doc.meter_num > 0 && doc.meter_den > 0) {
            st.bar_sec = static_cast<double>(doc.meter_num) * 4.0 / doc.meter_den * beat;
            st.bars = cursor / st.bar_sec;
            st.bar_fit_error = cursor - std::round(st.bars) * st.bar_sec;
        }
        *stats = std::move(st);
    }
    return audio;
}

// ── 兼容入口 ───────────────────────────────────────────────────────
std::vector<float> render_rcp_unified(std::string_view content,
                                      const std::vector<double>& fallback_harmonics,
                                      int sample_rate,
                                      std::vector<HarmonicSustain>* sustain_out,
                                      size_t* note_count)
{
    RcpDocument doc = parse_rcp(content);

    if (!doc.has_harmonics && !doc.has_acoustic && !fallback_harmonics.empty()) {
        doc.harmonics = fallback_harmonics;
        doc.has_harmonics = true;
    }

    RenderOptions opt;
    opt.sample_rate = sample_rate;
    auto audio = render_document(doc, opt, nullptr);

    if (sustain_out) *sustain_out = doc.sustain;
    if (note_count)  *note_count = doc.notes.size();
    return audio;
}

bool render_rcp_to_wav(std::string_view content,
                       const std::vector<double>& fallback_harmonics,
                       std::string_view out_path,
                       const RenderOptions& opt)
{
    RcpDocument doc = parse_rcp(content);
    if (!doc.has_harmonics && !doc.has_acoustic && !fallback_harmonics.empty()) {
        doc.harmonics = fallback_harmonics;
        doc.has_harmonics = true;
    }

    auto audio = render_document(doc, opt, nullptr);
    if (audio.empty()) return false;
    return write_wav(out_path, audio, opt.sample_rate, opt.stereo ? 2 : 1);
}
