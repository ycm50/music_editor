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

/// 插值正弦表: 比逐样本 std::sin 快一个数量级, 误差 < -140 dB
struct SinTable {
    static constexpr int N = 1 << 14;
    std::vector<double> t;
    SinTable() : t(static_cast<size_t>(N) + 1)
    {
        for (int i = 0; i <= N; ++i)
            t[static_cast<size_t>(i)] = std::sin(2.0 * kPi * i / N);
    }
    inline double at(double phase01) const   // phase01 ∈ [0,1)
    {
        const double x = phase01 * N;
        const int i = static_cast<int>(x);
        const double fr = x - i;
        const double a = t[static_cast<size_t>(i)];
        const double b = t[static_cast<size_t>(i) + 1];
        return a + (b - a) * fr;
    }
};
const SinTable& sin_table()
{
    static const SinTable s;
    return s;
}

/// 声部槽: 记录 [start,end) 区间, 用于统计最大同时发声数
struct Slot { double start = 0.0; double end = -1.0; };

// ── 共鸣体: 模拟 biquad 峰值滤波器幅频响应 (与采样率无关) ────────
double peaking_gain(double f, double fr, double q, double gain_db)
{
    if (gain_db == 0.0 || fr <= 0.0 || q <= 0.0) return 1.0;
    const double A  = std::pow(10.0, gain_db / 40.0);
    const double w0 = 2.0 * kPi * fr;
    const double w  = 2.0 * kPi * f;
    const double re = w0 * w0 - w * w;
    const double im_num = (A / q) * w0 * w;
    const double im_den = (1.0 / (A * q)) * w0 * w;
    const double num = std::sqrt(re * re + im_num * im_num);
    const double den = std::sqrt(re * re + im_den * im_den);
    return den > 1e-30 ? num / den : 1.0;
}

/// 共鸣体总增益: 共振峰乘积 × 低频滚降 (音板/琴箱尺寸造成的截止)
double body_gain(double f, const AcousticParams& a)
{
    double g = 1.0;
    for (const auto& r : a.body)
        g *= peaking_gain(f, r.freq_hz, r.q, r.gain_db);
    if (a.body_hp_hz > 0.0 && f < a.body_hp_hz)
        g *= std::pow(f / a.body_hp_hz, a.body_hp_db_oct / 6.0206);
    return g;
}

/// 该音实际使用的非谐性 B(f) = B·(f/ref)^alpha
double inharmonicity_at(double f0, const AcousticParams& a)
{
    if (a.inharmonicity <= 0.0) return 0.0;
    if (a.inharm_alpha == 0.0 || a.inharm_ref_hz <= 0.0) return a.inharmonicity;
    return a.inharmonicity * std::pow(f0 / a.inharm_ref_hz, a.inharm_alpha);
}

/// 阻尼律 → 第 n 分音的 tau (n 从 1 起)
double partial_tau(int n, const AcousticParams& a)
{
    if (a.damping_law == DampingLaw::TwoTerm) {
        const double inv = 1.0 / a.decay + a.damping_n2 * (double(n) * n - 1.0);
        if (inv > 0.0) return 1.0 / inv;
        return a.decay;
    }
    const double t = a.decay / std::pow(static_cast<double>(n), a.damping_exp);
    return t > 0.0 ? t : a.decay;
}

} // namespace

// ── 音色查找 (旧式裸谐波表) ───────────────────────────────────────
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

// ── 逐音覆盖合并 ───────────────────────────────────────────────────
AcousticParams resolve_acoustic(const AcousticParams& base, const NoteOverrides& ov)
{
    AcousticParams a = base;
    if (ov.has_atk) a.attack        = ov.atk;
    if (ov.has_dec) { a.decay = ov.dec;
                      a.partial_decay.clear();        // 逐音 tau 覆盖逐分音表
                      a.damping_law = DampingLaw::PowerExp; a.damping_exp = 0.0; }
    if (ov.has_sus) a.sustain       = ov.sus;
    if (ov.has_rel) a.release       = ov.rel;
    if (ov.has_br)  { a.brightness  = ov.br; a.tilt_db_oct = 0.0; }
    if (ov.has_B)   a.inharmonicity = ov.B;
    if (ov.has_pos) a.strike_pos    = ov.pos;
    if (ov.has_tilt){ a.tilt_db_oct = ov.tilt; }
    if (ov.has_damper) a.damper     = ov.damper;
    if (ov.has_ring)   a.ring       = ov.ring;
    if (ov.has_n2)  { a.damping_n2  = ov.n2; a.damping_law = DampingLaw::TwoTerm; }
    if (ov.has_uni)    a.unison     = ov.uni > 0 ? ov.uni : 1;
    if (ov.has_detune) a.detune_cents = ov.detune;
    if (ov.has_pan)    a.pan        = ov.pan;
    return a;
}

// ── 分音表 (物理模型核心) ─────────────────────────────────────────
std::vector<TonePartial> build_partials(double f0, const AcousticParams& a,
                                        int sample_rate, double velocity)
{
    std::vector<TonePartial> out;
    if (!(f0 > 0.0) || sample_rate <= 0) return out;

    const double sr  = sample_rate;
    const double lim = sr * 0.49;                       // 留一点抗混叠余量
    const double B   = inharmonicity_at(f0, a);
    const double vel = std::clamp(velocity, 0.0, 4.0);
    const double v01 = std::clamp(vel, 0.0, 1.0);

    // 力度 → 音色: 越轻越暗 (piano 的 pp 比 ff 暗好几个 dB/oct)
    const double vel_tilt_oct = a.vel_tilt * (1.0 - v01);
    const bool coherent = (a.phase == PhaseMode::Coherent);

    auto vel_tilt = [&](double f) {
        if (vel_tilt_oct <= 0.0) return 1.0;
        return std::pow(10.0, -vel_tilt_oct * std::log2(f / f0) / 20.0);
    };

    // ① 显式分音表 (钟/马林巴/定音鼓等非谐波乐器)
    if (!a.partial_table.empty()) {
        for (const auto& s : a.partial_table) {
            const double f = f0 * s.ratio;
            if (f >= lim) continue;
            TonePartial p;
            p.freq = f;
            p.amp  = s.amp * body_gain(f, a) * vel_tilt(f);
            p.tau  = s.tau;
            p.inverted = false;
            if (p.amp > 0.0) out.push_back(p);
            else out.push_back(p);
        }
        return out;
    }

    // ② 谐波列 + 非谐性 + 激励点梳状 + 共鸣体
    const int nmax = std::clamp(a.max_partials, 1, 1024);
    for (int n = 1; n <= nmax; ++n) {
        const double f = partial_frequency(f0, n, B);
        if (f >= lim) break;

        // 源频谱包络: tilt(dB/oct) 优先, 否则 br^(n-1)
        double env_amp = (a.tilt_db_oct > 0.0)
                             ? std::pow(10.0, -a.tilt_db_oct * std::log2(f / f0) / 20.0)
                             : std::pow(a.brightness, n - 1);

        // 激励点 (拨/击弦位置) 梳状零点 + 1/n^p 包络
        double comb = 1.0;
        bool inverted = false;
        if (a.strike_pos > 1e-9) {
            const double s = std::sin(kPi * static_cast<double>(n) * a.strike_pos);
            comb = std::fabs(s);
            inverted = coherent && (s < 0.0);
            if (a.strike_p > 0.0)
                comb *= std::pow(static_cast<double>(n), 1.0 - a.strike_p);
        }

        const double body = body_gain(f, a);
        const double total_env = env_amp * body * vel_tilt(f);
        if (total_env < a.partial_floor) break;   // 听阈以下直接停 (按包络判, 不受梳状零点影响)

        TonePartial p;
        p.freq  = f;
        p.amp   = total_env * comb;
        p.tau   = (!a.partial_decay.empty() && static_cast<size_t>(n - 1) < a.partial_decay.size())
                      ? a.partial_decay[static_cast<size_t>(n - 1)]
                      : partial_tau(n, a);
        p.inverted = inverted;
        out.push_back(p);
    }
    return out;
}

// ── 实际发声长度 ──────────────────────────────────────────────────
// 有制音器: 按键时值 + 齿毡阻尼 (6.5τ ≈ -56 dB)
// 无制音器: 累积分音能量衰减到 -60 dB 的时间 (上限 ring); 这样长余音乐器
//           (钟/竖琴/锣) 不会为了一个八分音符渲染十几秒, 高次分音也自然先消失。
double note_duration(double gate, const AcousticParams& a, double f0, int sr, double vel)
{
    if (gate <= 0.0) return 0.0;
    if (a.damper > 0.0) return gate + std::min(1.2, a.damper * 6.5);
    if (!(f0 > 0.0)) return gate;
    if (a.sustain > 0.98) return gate + 0.15;

    std::vector<TonePartial> ps = build_partials(f0, a, sr, vel);
    if (ps.empty()) return gate;
    double e0 = 0.0;
    for (const auto& p : ps) e0 += p.amp * p.amp;
    if (!(e0 > 0.0)) return gate;

    const double thr = e0 * 1e-6;          // -60 dB
    const double dt = 0.25;
    double last = gate;
    for (double t = 0.0; t < a.ring; t += dt) {
        double e = 0.0;
        for (const auto& p : ps) {
            const double x = p.amp * std::exp(-t / std::max(1e-6, p.tau));
            e += x * x;
        }
        if (e < thr) break;
        last = t;
    }
    return std::max(gate, std::min(a.ring, last + dt));
}

// ── 律制与伸展调音 ────────────────────────────────────────────────
double apply_temperament(double freq, double tonic, Temperament t, double stretch)
{
    if (!(freq > 0.0)) return freq;

    if (t != Temperament::Equal12 && tonic > 0.0) {
        static const double just[12] = {
            1.0, 16.0/15.0, 9.0/8.0, 6.0/5.0, 5.0/4.0, 4.0/3.0,
            45.0/32.0, 3.0/2.0, 8.0/5.0, 5.0/3.0, 9.0/5.0, 15.0/8.0 };
        static const double pyth[12] = {
            1.0, 256.0/243.0, 9.0/8.0, 32.0/27.0, 81.0/64.0, 4.0/3.0,
            729.0/512.0, 3.0/2.0, 128.0/81.0, 27.0/16.0, 16.0/9.0, 243.0/128.0 };
        const double s = 12.0 * std::log2(freq / tonic);
        const double oct = std::floor(s / 12.0);
        int idx = static_cast<int>(std::llround(s - oct * 12.0));
        idx = ((idx % 12) + 12) % 12;
        const double* tbl = (t == Temperament::Just) ? just : pyth;
        freq = tonic * std::pow(2.0, oct) * tbl[idx];
    }

    if (stretch > 0.0) {
        // Railsback 曲线的近似: 以 261.63 Hz 为中心, 偏差随八度距离二次增长
        const double o = std::log2(freq / 261.625565);
        double cents = 1.6 * o * std::fabs(o);
        cents = std::clamp(cents, -60.0, 60.0) * stretch;
        freq *= std::pow(2.0, cents / 1200.0);
    }
    return freq;
}

// ── 显式谐波表 (文档给出) → 分音表 ────────────────────────────────
namespace {
std::vector<TonePartial> partials_from_amps(const std::vector<double>& amps, double f0,
                                            const AcousticParams& a, int sr, double vel)
{
    std::vector<TonePartial> out;
    const double B = inharmonicity_at(f0, a);
    const double lim = sr * 0.49;
    const double v01 = std::clamp(vel, 0.0, 1.0);
    const double vel_tilt_oct = a.vel_tilt * (1.0 - v01);
    for (size_t k = 0; k < amps.size(); ++k) {
        const int n = static_cast<int>(k) + 1;
        const double f = partial_frequency(f0, n, B);
        if (f >= lim) continue;
        double amp = amps[k] * body_gain(f, a);
        if (vel_tilt_oct > 0.0)
            amp *= std::pow(10.0, -vel_tilt_oct * std::log2(f / f0) / 20.0);
        if (amp <= 0.0) continue;
        TonePartial p;
        p.freq = f;
        p.amp  = amp;
        p.tau  = (!a.partial_decay.empty() && k < a.partial_decay.size())
                     ? a.partial_decay[k]
                     : partial_tau(n, a);
        out.push_back(p);
    }
    return out;
}
} // namespace

// ── 单音合成 ───────────────────────────────────────────────────────
std::vector<float> generate_tone(double frequency,
                                 double duration_sec,
                                 const std::vector<double>& harmonics,
                                 int sample_rate,
                                 const AcousticParams& acoustic,
                                 const NoteOverrides& ov,
                                 double velocity,
                                 uint32_t seed)
{
    if (duration_sec <= 0.0 || sample_rate <= 0) return {};
    AcousticParams a = resolve_acoustic(acoustic, ov);
    const double sr  = sample_rate;
    const double vel = std::clamp(velocity, 0.0, 4.0);

    const double gate     = duration_sec;
    const double tone_len = note_duration(gate, a, frequency, sample_rate, vel);
    const long   num      = static_cast<long>(sr * tone_len);
    if (num <= 0) return {};

    // 休止符 / 无基频 → 真静音
    if (frequency <= 0.0)
        return std::vector<float>(static_cast<size_t>(num), 0.0f);

    std::vector<TonePartial> ps = harmonics.empty()
                                      ? build_partials(frequency, a, sample_rate, vel)
                                      : partials_from_amps(harmonics, frequency, a, sample_rate, vel);
    if (ps.empty())
        return std::vector<float>(static_cast<size_t>(num), 0.0f);

    // 幅度标定: 用分音平方和的平方根做音频域标定 (随机相位下 RMS ≈ 1/√2)
    double sumsq = 0.0;
    for (const auto& p : ps) sumsq += p.amp * p.amp;
    const double norm = 1.0 / std::sqrt(std::max(sumsq, 1e-12));

    const double v01 = std::min(vel, 1.0);
    // 力度越大起音越硬 (琴槌接触时间变短)
    const double atk = std::max(1e-5, a.attack * (1.0 - a.vel_attack * (1.0 - v01)));
    const int  atk_n = std::clamp(static_cast<int>(atk * sr), 1,
                                  std::max(1, static_cast<int>(num / 2)));
    const long gate_n = static_cast<long>(gate * sr);
    const bool two_stage = (a.decay2_beta > 0.0 && a.decay2_beta < 1.0);
    const bool has_damper = (a.damper > 0.0);
    const bool coherent = (a.phase == PhaseMode::Coherent);

    const SinTable& st = sin_table();
    const int uni = std::clamp(a.unison, 1, 8);
    const double uni_gain = 1.0 / std::sqrt(static_cast<double>(uni));

    std::vector<float> out(static_cast<size_t>(num), 0.0f);

    // 每根分音: 一个振荡器 + 拍频包络 (同音弦组等效) + 分段紧循环 (无内部分支)
    size_t k = 0;
    for (const auto& p : ps) {
        ++k;
        if (p.amp <= 0.0) continue;
        const double f = p.freq;
        if (f >= sr * 0.4999) continue;

        // 同音弦组: U 根微失谐弦叠加 ≈ 载波 × (1 + (U-1)cos(2πδt))/U
        // 只需一个振荡器即可得到真实的拍频闪烁, 且保持 RMS 不变。
        const double beat_off   = (uni > 1) ? 1.0 / uni : 1.0;
        const double beat_depth = (uni > 1) ? static_cast<double>(uni - 1) / uni : 0.0;
        const double beat_gain  = 1.0 / std::sqrt(beat_off * beat_off + 0.5 * beat_depth * beat_depth);
        const double beat_inc   = (uni > 1 && a.detune_cents > 0.0)
                                      ? p.freq * (std::pow(2.0, a.detune_cents / 1200.0) - 1.0) / sr
                                      : 0.0;
        double beat_phase = hash_unit(0x85ebca6bu ^ (static_cast<uint32_t>(k) * 2246822519u));

        const double phase_inc = f / sr;
        double phase = coherent
                           ? (p.inverted ? 0.5 : 0.0)
                           : hash_unit(0x9e3779b9u ^ (static_cast<uint32_t>(k) * 2654435761u));

        const double step   = std::exp(-1.0 / (sr * std::max(1e-6, p.tau)));
        const double step_f = two_stage
                                  ? std::exp(-1.0 / (sr * std::max(1e-6, p.tau * a.decay2_ratio)))
                                  : 1.0;
        const double dstep  = has_damper ? std::exp(-1.0 / (sr * a.damper)) : 1.0;
        const double A0 = p.amp * vel * norm;

        double dec = 1.0, dec_f = 1.0, dmp = 1.0;
        double sn_1 = std::sin(2.0 * kPi * (phase - phase_inc));
        double sn   = std::sin(2.0 * kPi * phase);
        const double twocos = 2.0 * std::cos(2.0 * kPi * phase_inc);
        long resync = 4096;
        const auto& lut = sin_table();

        const long seg2_end = has_damper ? std::min<long>(num, std::max<long>(gate_n, atk_n)) : num;
        const bool damp_in_seg2 = has_damper && (atk_n >= gate_n);
        if (damp_in_seg2 && atk_n > gate_n)
            dmp = std::exp(-static_cast<double>(atk_n - gate_n) / (sr * a.damper));

        long s = 0;
        // ① 起音段 (平方上升, 与力度相关)
        for (; s < atk_n; ++s) {
            const double q = static_cast<double>(s) / static_cast<double>(atk_n);
            const double env = q * q;
            out[static_cast<size_t>(s)] += static_cast<float>(A0 * env * sn);
            const double snx = twocos * sn - sn_1;
            sn_1 = sn; sn = snx;
            phase += phase_inc; if (phase >= 1.0) phase -= 1.0;
            if (--resync == 0) {
                resync = 4096;
                sn_1 = std::sin(2.0 * kPi * (phase - phase_inc));
                sn   = std::sin(2.0 * kPi * phase);
            }
        }
        // ② 稳态 / 衰减段 (单指数或双指数)
        if (two_stage) {
            for (; s < seg2_end; ++s) {
                double env = a.sustain + (1.0 - a.sustain)
                                            * ((1.0 - a.decay2_beta) * dec_f + a.decay2_beta * dec);
                dec *= step; dec_f *= step_f;
                double mod = 1.0;
                if (beat_depth > 0.0) { mod = (beat_off + beat_depth * lut.at(beat_phase)) * beat_gain;
                                        beat_phase += beat_inc; if (beat_phase >= 1.0) beat_phase -= 1.0; }
                if (damp_in_seg2) { env *= dmp; dmp *= dstep; }
                out[static_cast<size_t>(s)] += static_cast<float>(A0 * env * mod * sn);
                const double snx = twocos * sn - sn_1;
                sn_1 = sn; sn = snx;
                phase += phase_inc; if (phase >= 1.0) phase -= 1.0;
                if (--resync == 0) {
                    resync = 4096;
                    sn_1 = std::sin(2.0 * kPi * (phase - phase_inc));
                    sn   = std::sin(2.0 * kPi * phase);
                }
            }
        } else {
            for (; s < seg2_end; ++s) {
                double env = a.sustain + (1.0 - a.sustain) * dec;
                dec *= step;
                double mod = 1.0;
                if (beat_depth > 0.0) { mod = (beat_off + beat_depth * lut.at(beat_phase)) * beat_gain;
                                        beat_phase += beat_inc; if (beat_phase >= 1.0) beat_phase -= 1.0; }
                if (damp_in_seg2) { env *= dmp; dmp *= dstep; }
                out[static_cast<size_t>(s)] += static_cast<float>(A0 * env * mod * sn);
                const double snx = twocos * sn - sn_1;
                sn_1 = sn; sn = snx;
                phase += phase_inc; if (phase >= 1.0) phase -= 1.0;
                if (--resync == 0) {
                    resync = 4096;
                    sn_1 = std::sin(2.0 * kPi * (phase - phase_inc));
                    sn   = std::sin(2.0 * kPi * phase);
                }
            }
        }
        // ③ 制音器段 (松键/离弓后按齿毡阻尼衰减)
        if (has_damper && s < num) {
            if (!damp_in_seg2) dmp = 1.0;
            for (; s < num; ++s) {
                const double base = two_stage
                                        ? ((1.0 - a.decay2_beta) * dec_f + a.decay2_beta * dec)
                                        : dec;
                const double env = (a.sustain + (1.0 - a.sustain) * base) * dmp;
                dec *= step; dec_f *= step_f; dmp *= dstep;
                double mod = 1.0;
                if (beat_depth > 0.0) { mod = (beat_off + beat_depth * lut.at(beat_phase)) * beat_gain;
                                        beat_phase += beat_inc; if (beat_phase >= 1.0) beat_phase -= 1.0; }
                out[static_cast<size_t>(s)] += static_cast<float>(A0 * env * mod * sn);
                const double snx = twocos * sn - sn_1;
                sn_1 = sn; sn = snx;
                phase += phase_inc; if (phase >= 1.0) phase -= 1.0;
                if (--resync == 0) {
                    resync = 4096;
                    sn_1 = std::sin(2.0 * kPi * (phase - phase_inc));
                    sn   = std::sin(2.0 * kPi * phase);
                }
            }
        }
    }

    // ── 起音噪声 (弓毛/气流/击弦) ────────────────────────────────
    // 物理上噪声必须先激励弦/管的同一组模态再辐射出去, 因此这里用"模态噪声":
    // 用与音色相同的分音频率做共振, 再混入少量宽带分量。
    if (a.noise > 0.0) {
        const double namp = a.noise * (1.0 - a.vel_noise * (1.0 - v01)) * v01 * 0.6;
        const long nn = std::min<long>(num, static_cast<long>(a.noise_len * sr));
        if (namp > 0.0 && nn > 0) {
            const int K = static_cast<int>(std::min<size_t>(ps.size(), 24));
            std::vector<double> w(static_cast<size_t>(K)), inc(static_cast<size_t>(K)),
                                ph(static_cast<size_t>(K));
            double wsum = 0.0;
            for (int i = 0; i < K; ++i) { w[static_cast<size_t>(i)] = std::fabs(ps[static_cast<size_t>(i)].amp); wsum += w[static_cast<size_t>(i)]; }
            if (wsum <= 0.0) wsum = 1.0;
            for (int i = 0; i < K; ++i) {
                w[static_cast<size_t>(i)] /= wsum;
                inc[static_cast<size_t>(i)] = ps[static_cast<size_t>(i)].freq / sr;
                ph[static_cast<size_t>(i)]  = hash_unit(seed * 40503u + static_cast<uint32_t>(i) * 2246822519u);
            }
            const long fade_in = std::max<long>(1, static_cast<long>(0.0005 * sr));
            for (long s = 0; s < nn; ++s) {
                const double q = static_cast<double>(s) / static_cast<double>(nn);
                double envn = (1.0 - q) * (1.0 - q);
                if (s < fade_in) envn *= static_cast<double>(s) / static_cast<double>(fade_in);
                const double r = hash_unit(seed + 0x51ed2701u + static_cast<uint32_t>(s)) * 2.0 - 1.0;
                double modal = 0.0;
                for (int i = 0; i < K; ++i) {
                    modal += w[static_cast<size_t>(i)] * st.at(ph[static_cast<size_t>(i)]);
                    ph[static_cast<size_t>(i)] += inc[static_cast<size_t>(i)];
                    if (ph[static_cast<size_t>(i)] >= 1.0) ph[static_cast<size_t>(i)] -= 1.0;
                }
                out[static_cast<size_t>(s)] += static_cast<float>(namp * envn * (0.75 * modal + 0.25 * r));
            }
        }
    }

    // ── 末尾自然收尾 (cos²): 至少 1.5 ms, 保证零跳变 ─────────────
    const long rel_n = std::min<long>(num,
                                      static_cast<long>(std::max(a.release, 0.0015) * sr));
    for (long s = 0; s < rel_n; ++s) {
        const double p = static_cast<double>(s) / static_cast<double>(rel_n);
        const double w = 0.5 - 0.5 * std::cos(kPi * p);
        out[static_cast<size_t>(num - 1 - s)] *= static_cast<float>(w);
    }
    return out;
}

// ── 单音实测指标 ───────────────────────────────────────────────────
ToneAnalysis analyze_tone(const std::vector<float>& tone, int sample_rate,
                          const std::vector<double>& partial_freqs, double floor_db)
{
    ToneAnalysis r;
    if (tone.empty() || sample_rate <= 0) return r;
    const long n = static_cast<long>(tone.size());
    const double sr = sample_rate;

    float peak = 0.0f;
    for (float v : tone) peak = std::max(peak, std::fabs(v));
    if (peak <= 1e-9f) return r;

    // 起音 10% → 90%: 参照"前 200 ms 的峰值"(起音段的峰值),
    // 否则双弦拍频在几百毫秒后的极大值会把起音时间算成 0.4 s。
    const long ref_n = std::min<long>(n, static_cast<long>(0.2 * sr));
    float peak_ref = 0.0f;
    for (long s = 0; s < ref_n; ++s)
        peak_ref = std::max(peak_ref, std::fabs(tone[static_cast<size_t>(s)]));
    if (peak_ref <= 1e-9f) peak_ref = peak;
    long i10 = -1, i90 = -1;
    for (long s = 0; s < n; ++s) {
        const float av = std::fabs(tone[static_cast<size_t>(s)]);
        if (i10 < 0 && av >= 0.1f * peak_ref) i10 = s;
        if (av >= 0.9f * peak_ref) { i90 = s; break; }
    }
    if (i10 >= 0 && i90 >= i10) r.attack_sec = static_cast<double>(i90 - i10) / sr;

    // RMS 包络 (10 ms) → T30 (由 -5 dB 到 -35 dB 的耗时)
    const long win = std::max<long>(1, static_cast<long>(0.010 * sr));
    std::vector<double> rms;
    rms.reserve(static_cast<size_t>(n / win + 1));
    for (long s = 0; s + win <= n; s += win) {
        double acc = 0.0;
        for (long i = 0; i < win; ++i) {
            const double v = tone[static_cast<size_t>(s + i)];
            acc += v * v;
        }
        rms.push_back(std::sqrt(acc / win));
    }
    if (!rms.empty()) {
        double mx = 0.0;
        size_t imx = 0;
        for (size_t i = 0; i < rms.size(); ++i)
            if (rms[i] > mx) { mx = rms[i]; imx = i; }
        double t5 = -1.0, t35 = -1.0;
        for (size_t i = imx; i < rms.size() && mx > 1e-12; ++i) {
            const double db = 20.0 * std::log10(std::max(rms[i], 1e-12) / mx);
            if (t5 < 0.0 && db <= -5.0)  t5  = static_cast<double>(i) * win / sr;
            if (db <= -35.0) { t35 = static_cast<double>(i) * win / sr; break; }
        }
        if (t5 >= 0.0 && t35 > t5) {
            r.t30_sec = t35 - t5;
        } else if (t5 >= 0.0) {
            r.t30_sec = static_cast<double>(rms.size() - imx) * win / sr;
            r.truncated = true;
        }
        // 末段仍在发声 → 被截断
        const double end_rms = rms.back();
        if (mx > 1e-12 && 20.0 * std::log10(std::max(end_rms, 1e-12) / mx) > -40.0)
            r.truncated = true;
    }

    // 实测频谱重心与分音数 (Goertzel + Hann)
    const long L = std::min<long>(n, static_cast<long>(0.15 * sr));
    if (L > 32 && !partial_freqs.empty()) {
        std::vector<double> winv(static_cast<size_t>(L));
        for (long i = 0; i < L; ++i)
            winv[static_cast<size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (L - 1));
        std::vector<double> mags(partial_freqs.size(), 0.0);
        double maxm = 0.0;
        for (size_t k = 0; k < partial_freqs.size(); ++k) {
            const double f = partial_freqs[k];
            if (!(f > 0.0) || f >= sr * 0.5) continue;
            const double c = 2.0 * std::cos(2.0 * kPi * f / sr);
            double s1 = 0.0, s2 = 0.0;
            for (long i = 0; i < L; ++i) {
                const double s0 = static_cast<double>(tone[static_cast<size_t>(i)])
                                  * winv[static_cast<size_t>(i)] + c * s1 - s2;
                s2 = s1; s1 = s0;
            }
            const double mag = std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / L;
            mags[k] = mag;
            maxm = std::max(maxm, mag);
        }
        if (maxm > 1e-12) {
            const double thr = std::pow(10.0, floor_db / 20.0) * maxm;
            double num = 0.0, den = 0.0;
            int cnt = 0;
            for (size_t k = 0; k < mags.size(); ++k) {
                if (mags[k] < thr) continue;
                num += mags[k] * partial_freqs[k];
                den += mags[k];
                ++cnt;
            }
            if (den > 1e-12) {
                r.centroid_hz = num / den;
                r.partials = cnt;
            }
        }
    }
    return r;
}

// ── 响度 (简化 BS.1770: K 加权 + 门限) ────────────────────────────
namespace {
struct Biquad { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };

Biquad design_high_shelf(double f0, double q, double gain_db, double sr)
{
    const double A = std::pow(10.0, gain_db / 40.0);
    const double w0 = 2.0 * kPi * f0 / sr;
    const double cs = std::cos(w0), sn = std::sin(w0);
    const double alpha = sn / (2.0 * q);
    const double sq = 2.0 * std::sqrt(A) * alpha;
    Biquad f;
    const double a0 = (A + 1) - (A - 1) * cs + sq;
    f.b0 =  A * ((A + 1) + (A - 1) * cs + sq) / a0;
    f.b1 = -2 * A * ((A - 1) + (A + 1) * cs) / a0;
    f.b2 =  A * ((A + 1) + (A - 1) * cs - sq) / a0;
    f.a1 =  2 * ((A - 1) - (A + 1) * cs) / a0;
    f.a2 = ((A + 1) - (A - 1) * cs - sq) / a0;
    return f;
}

Biquad design_high_pass(double f0, double q, double sr)
{
    const double w0 = 2.0 * kPi * f0 / sr;
    const double cs = std::cos(w0), sn = std::sin(w0);
    const double alpha = sn / (2.0 * q);
    Biquad f;
    const double a0 = 1.0 + alpha;
    f.b0 = (1.0 + cs) / 2.0 / a0;
    f.b1 = -(1.0 + cs) / a0;
    f.b2 = (1.0 + cs) / 2.0 / a0;
    f.a1 = -2.0 * cs / a0;
    f.a2 = (1.0 - alpha) / a0;
    return f;
}

inline double bq(const Biquad& f, double x, double& z1, double& z2)
{
    const double y = f.b0 * x + z1;
    z1 = f.b1 * x - f.a1 * y + z2;
    z2 = f.b2 * x - f.a2 * y;
    return y;
}
} // namespace

double measure_lufs(const std::vector<float>& mono, int sample_rate)
{
    if (mono.empty() || sample_rate <= 0) return -120.0;
    const double sr = sample_rate;
    const Biquad shelf = design_high_shelf(1681.9744509555319, 0.7071752369554196, 3.999843853973347, sr);
    const Biquad hpf   = design_high_pass(38.13547087602444, 0.5003270373238773, sr);

    std::vector<double> k(mono.size());
    double z1 = 0, z2 = 0;
    for (size_t i = 0; i < mono.size(); ++i)
        k[i] = bq(shelf, mono[i], z1, z2);
    z1 = z2 = 0;
    for (size_t i = 0; i < mono.size(); ++i)
        k[i] = bq(hpf, k[i], z1, z2);

    const long win  = std::max<long>(1, static_cast<long>(0.400 * sr));
    const long step = std::max<long>(1, static_cast<long>(0.100 * sr));
    std::vector<double> loud;
    std::vector<double> zs;
    for (long s = 0; s + win <= static_cast<long>(k.size()); s += step) {
        double acc = 0.0;
        for (long i = 0; i < win; ++i) {
            const double v = k[static_cast<size_t>(s + i)];
            acc += v * v;
        }
        const double z = acc / win;
        zs.push_back(z);
        loud.push_back(-0.691 + 10.0 * std::log10(std::max(z, 1e-20)));
    }
    if (loud.empty()) {
        // 比 400 ms 还短: 直接整段
        double acc = 0.0;
        for (double v : k) acc += v * v;
        return -0.691 + 10.0 * std::log10(std::max(acc / std::max<double>(1.0, k.size()), 1e-20));
    }
    // 绝对门限 -70 LUFS
    double sum = 0.0; int cnt = 0;
    for (size_t i = 0; i < loud.size(); ++i)
        if (loud[i] > -70.0) { sum += zs[i]; ++cnt; }
    if (cnt == 0) return -120.0;
    const double rel_gate = -0.691 + 10.0 * std::log10(sum / cnt) - 10.0;
    sum = 0.0; cnt = 0;
    for (size_t i = 0; i < loud.size(); ++i)
        if (loud[i] > -70.0 && loud[i] > rel_gate) { sum += zs[i]; ++cnt; }
    if (cnt == 0) return -120.0;
    return -0.691 + 10.0 * std::log10(std::max(sum / cnt, 1e-20));
}

// ── 混响 (Schroeder 反馈网络, 参数化 RT60 / 阻尼 / 预延迟 / 宽度) ──
namespace {

struct Comb {
    std::vector<double> buf;
    int idx = 0;
    double store = 0.0;   // 环路低通状态
    double d = 0.0;       // 低通系数 (高频吸收)
    double g = 0.0;       // 环路增益 (由 RT60 与延时决定)
    explicit Comb(int n) : buf(static_cast<size_t>(std::max(1, n)), 0.0) {}
    inline double process(double x)
    {
        const double y = buf[static_cast<size_t>(idx)];
        store = (1.0 - d) * y + d * store;
        buf[static_cast<size_t>(idx)] = x + store * g;
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
        const double y = buf[static_cast<size_t>(idx)];
        const double v = x + g * y;
        buf[static_cast<size_t>(idx)] = v;
        if (++idx >= static_cast<int>(buf.size())) idx = 0;
        return y - g * v;
    }
};

/// 预延迟线
struct DelayLine {
    std::vector<double> buf;
    size_t idx = 0;
    explicit DelayLine(int n) : buf(static_cast<size_t>(std::max(1, n)), 0.0) {}
    inline double push(double x)
    {
        const double y = buf[idx];
        buf[idx] = x;
        if (++idx >= buf.size()) idx = 0;
        return y;
    }
};

/// 单声道混响网: 8 梳状 + 4 全通 + 预延迟
struct ReverbNet {
    std::vector<Comb> combs;
    std::vector<Allpass> aps;
    DelayLine pre;
    double wet_scale = 1.0;

    ReverbNet(const double* comb_ms, int nc, const double* ap_ms, int na,
              double rt60, double damp_hz, double sr, int pre_samples)
        : pre(pre_samples)
    {
        double gsum = 0.0;
        for (int i = 0; i < nc; ++i) {
            const double t = comb_ms[i] / 1000.0;
            combs.emplace_back(static_cast<int>(t * sr));
            combs.back().g = std::pow(10.0, -3.0 * t / std::max(0.05, rt60));
            gsum += combs.back().g;
        }
        const double d = (damp_hz > 0.0)
                             ? std::clamp(std::exp(-2.0 * kPi * damp_hz / sr), 0.0, 0.9999)
                             : 0.0;
        for (auto& c : combs) c.d = d;
        for (int i = 0; i < na; ++i)
            aps.emplace_back(static_cast<int>(ap_ms[i] / 1000.0 * sr));
        // 稳态增益归一: 并联梳状的直流增益 ≈ 1/(1-g), 用 (1-g_avg) 抵消
        const double gavg = gsum / std::max(1, nc);
        wet_scale = (1.0 - gavg) / std::max(1.0, static_cast<double>(nc) / 4.0);
    }

    inline double process(double x)
    {
        const double xd = pre.push(x);
        double acc = 0.0;
        for (auto& c : combs) acc += c.process(xd);
        acc *= wet_scale;
        for (auto& a : aps) acc = a.process(acc, 0.5);
        return acc;
    }
};

void apply_reverb(std::vector<float>& L, std::vector<float>& R, int sr, const RenderOptions& opt)
{
    const double wet = std::clamp(opt.reverb_wet, 0.0, 1.0);
    if (wet <= 0.0 || L.empty()) return;

    static const double cl_ms[8] = {29.7, 37.1, 41.1, 43.7, 47.9, 53.3, 59.3, 67.1};
    static const double cr_ms[8] = {31.3, 39.7, 43.9, 47.3, 51.7, 55.9, 61.9, 69.7};
    static const double al_ms[4] = {5.0, 1.7, 12.7, 8.3};
    static const double ar_ms[4] = {5.3, 1.9, 13.1, 8.7};

    const int pre = static_cast<int>(std::max(0.0, opt.reverb_predelay_ms) * sr / 1000.0);
    ReverbNet nl(cl_ms, 8, al_ms, 4, opt.reverb_rt60, opt.reverb_damp_hz, sr, pre);
    ReverbNet nr(cr_ms, 8, ar_ms, 4, opt.reverb_rt60, opt.reverb_damp_hz, sr, pre);

    const double width = std::clamp(opt.reverb_width, 0.0, 1.0);
    const double dry = 1.0 - 0.35 * wet;
    for (size_t i = 0; i < L.size(); ++i) {
        const double xl = L[i];
        const double xr = (i < R.size()) ? R[i] : xl;
        double wl = nl.process(xl);
        double wr = nr.process(xr);
        const double mid  = 0.5 * (wl + wr);
        const double side = 0.5 * (wl - wr) * width;
        wl = mid + side;
        wr = mid - side;
        L[i] = static_cast<float>(xl * dry + wl * wet);
        if (i < R.size()) R[i] = static_cast<float>(xr * dry + wr * wet);
    }
}

/// 一阶直流阻断 (物理上任何直流都是 bug; 比"减整段均值"更干净)
void dc_block(std::vector<float>& x, int sr)
{
    if (x.empty()) return;
    const double r = 1.0 - 2.0 * kPi * 5.0 / sr;   // ~5 Hz 转折
    double x1 = 0.0, y1 = 0.0;
    for (auto& v : x) {
        const double xn = v;
        const double yn = xn - x1 + r * y1;
        x1 = xn; y1 = yn;
        v = static_cast<float>(yn);
    }
}

/// 真峰值估计: 4 倍线性插值取最大
double true_peak_of(const std::vector<float>& x)
{
    double tp = 0.0;
    for (size_t i = 0; i + 1 < x.size(); ++i) {
        const double a = x[i], b = x[i + 1];
        for (int k = 0; k < 4; ++k) {
            const double t = k / 4.0;
            tp = std::max(tp, std::fabs(a + (b - a) * t));
        }
    }
    if (!x.empty()) tp = std::max(tp, static_cast<double>(std::fabs(x.back())));
    return tp;
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
    const std::vector<double>& doc_amps = doc.harmonics;   // 显式谐波表 (旧式, 可空)

    // ── 第一遍: 时间轴排版 (拍 → 绝对秒) ─────────────────────────
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
    double fmin = 0.0, fmax = 0.0;

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
            // 律制 / 伸展调音 (12-TET 之上修正)
            if (!doc.notes[k].rest && doc.notes[k].frequency > 0.0)
                e.freq = apply_temperament(doc.notes[k].frequency, doc.base_freq,
                                           acoustic.temperament, acoustic.stretch);
            e.vel = doc.notes[k].velocity;
            e.rest = doc.notes[k].rest;
            e.ov = doc.notes[k].ov;

            if (!e.rest && e.freq > 0.0) {
                fmin = (fmin == 0.0) ? e.freq : std::min(fmin, e.freq);
                fmax = std::max(fmax, e.freq);
            }

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

    // ── 总长度: 由"自然余音"决定 (末音不再被截断成静音) ──────────
    double max_end = 0.0;
    for (const auto& e : events) {
        if (e.rest) continue;
        const AcousticParams ea = resolve_acoustic(acoustic, e.ov);
        const double len = opt.hold_last_note
                               ? note_duration(e.dur, ea, e.freq, sr, e.vel)
                               : e.dur;
        max_end = std::max(max_end, e.start + len);
    }
    double tail = 0.05;
    if (opt.stereo && opt.reverb_wet > 0.0)
        tail = opt.reverb_predelay_ms / 1000.0 + opt.reverb_rt60 * 0.8;
    double total_sec = events.empty() ? 0.0 : (max_end + tail);
    size_t total = static_cast<size_t>(total_sec * sr);
    if (total == 0) {
        if (stats) { *stats = RenderStats{}; stats->warnings = doc.warnings; }
        return {};
    }

    const bool stereo = opt.stereo;
    std::vector<float> mixL(total, 0.0f);
    std::vector<float> mixR(stereo ? total : 0, 0.0f);

    const int nmax = std::clamp(acoustic.max_partials, 1, 1024);
    int idx = 0;
    if (stats) stats->notes.reserve(events.size());

    for (const auto& e : events) {
        NotePhysics np;
        np.index = idx++;
        np.frequency = e.freq;
        np.duration_sec = e.dur;
        np.velocity = e.vel;
        np.rest = e.rest;

        if (e.rest) {
            if (stats) { stats->notes.push_back(np); ++stats->rest_count; }
            continue;
        }

        const AcousticParams ea = resolve_acoustic(acoustic, e.ov);
        std::vector<TonePartial> ps = doc_amps.empty()
            ? build_partials(e.freq, ea, sr, e.vel)
            : partials_from_amps(doc_amps, e.freq, ea, sr, e.vel);

        auto tone = (e.freq > 0.0)
                        ? generate_tone(e.freq, e.dur, doc_amps, sr, acoustic, e.ov, e.vel,
                                        static_cast<uint32_t>(idx))
                        : std::vector<float>{};
        if (tone.empty() || e.freq <= 0.0) {
            if (stats) { stats->notes.push_back(np); }
            continue;
        }
        if (!opt.hold_last_note) {
            const size_t cut = static_cast<size_t>(e.dur * sr);
            if (tone.size() > cut) tone.resize(cut);
        } else if (stats) {
            // 硬切是咔哒的唯一来源: 音块首尾必须已经淡到 0 (合成末尾有最小淡出)
            if (std::fabs(tone.front()) > 1e-3f || std::fabs(tone.back()) > 1e-3f)
                ++stats->boundary_glitches;
        }

        // 短音等响补偿 (时域整合损失)
        double dur_gain = 1.0;
        if (opt.dur_compensate && e.dur < 0.2)
            dur_gain = std::min(1.6, std::pow(0.2 / std::max(0.02, e.dur), 0.15));

        // 声像: 固定 pan + 随音高展开 (钢琴键盘感) × 声像宽度 stereo
        double pan = std::clamp(ea.pan, -1.0, 1.0);
        if (ea.pan_pitch > 0.0 && fmax > fmin) {
            const double t01 = std::log2(e.freq / fmin) / std::log2(fmax / fmin);
            pan += (t01 - 0.5) * 2.0 * ea.pan_pitch;
        }
        if (ea.stereo > 0.0)
            pan *= std::clamp(1.0 + 0.8 * ea.stereo, 1.0, 1.8);   // 加宽立体声像
        pan = std::clamp(pan, -1.0, 1.0);
        const double ang = (pan + 1.0) * kPi / 4.0;
        const double gl = stereo ? std::cos(ang) : 1.0;
        const double gr = stereo ? std::sin(ang) : 0.0;

        const size_t off = static_cast<size_t>(e.start * sr);
        if (off >= total) { if (stats) stats->notes.push_back(np); continue; }
        const size_t cnt = std::min(tone.size(), total - off);
        for (size_t k = 0; k < cnt; ++k) {
            const float v = tone[k] * static_cast<float>(dur_gain);
            mixL[off + k] += static_cast<float>(v * gl);
            if (stereo) mixR[off + k] += static_cast<float>(v * gr);
        }

        if (stats) {
            // 模型侧指标
            double num = 0.0, den = 0.0;
            for (const auto& p : ps) { num += p.amp * p.freq; den += p.amp; }
            np.partials = static_cast<int>(ps.size());
            np.centroid_hz = den > 1e-12 ? num / den : 0.0;
            np.tau1_sec = (!ea.partial_decay.empty()) ? ea.partial_decay.front()
                          : (!ea.partial_table.empty() ? ea.partial_table.front().tau : ea.decay);
            np.b_used = inharmonicity_at(e.freq, ea);
            np.ring_sec = static_cast<double>(tone.size()) / sr;
            np.pan = pan;
            // 因"分音上限 max_partials"被裁剪的分音数 (超出 Nyquist 的部分不算,
            // 那些本来就该丢)。> 0 说明低音区的带宽被上限卡住, 可调大 maxp。
            {
                int disc = 0;
                const double B = np.b_used;
                const double v01 = std::min(e.vel, 1.0);
                const double vt = ea.vel_tilt * (1.0 - v01);
                for (int n = static_cast<int>(ps.size()) + 1; n <= nmax + 512; ++n) {
                    const double f = partial_frequency(e.freq, n, B);
                    if (f >= sr * 0.49) break;
                    double env_amp = (ea.tilt_db_oct > 0.0)
                        ? std::pow(10.0, -ea.tilt_db_oct * std::log2(f / e.freq) / 20.0)
                        : std::pow(ea.brightness, n - 1);
                    if (vt > 0.0) env_amp *= std::pow(10.0, -vt * std::log2(f / e.freq) / 20.0);
                    if (env_amp * body_gain(f, ea) < ea.partial_floor) break;
                    ++disc;
                }
                np.discarded = disc;
            }
            // 音频侧实测
            std::vector<double> pf;
            pf.reserve(ps.size());
            for (const auto& p : ps) pf.push_back(p.freq);
            if (pf.size() > 96) pf.resize(96);
            const ToneAnalysis ta = analyze_tone(tone, sr, pf);
            np.attack_sec = ta.attack_sec;
            np.t30_sec = ta.t30_sec;
            np.truncated = ta.truncated;
            np.meas_centroid_hz = ta.centroid_hz;
            np.meas_partials = ta.partials;
            np.tonic_cents = 1200.0 * std::log2(e.freq / doc.base_freq);
            stats->notes.push_back(np);
            ++stats->note_count;
            stats->discarded_total += np.discarded;
        }
    }

    // ── 去直流 (一阶高通) ────────────────────────────────────────
    dc_block(mixL, sr);
    if (stereo) dc_block(mixR, sr);

    // ── 混响 (在归一化之前, 让电平管理覆盖整个信号链) ────────────
    if (stereo && opt.reverb_wet > 0.0)
        apply_reverb(mixL, mixR, sr, opt);

    // ── 电平: 峰值 / 响度 ────────────────────────────────────────
    auto peak_of = [](const std::vector<float>& v) {
        double p = 0.0;
        for (float x : v) p = std::max(p, static_cast<double>(std::fabs(x)));
        return p;
    };
    const double peak = stereo ? std::max(peak_of(mixL), peak_of(mixR)) : peak_of(mixL);
    double gain = 1.0;
    if (opt.normalize == NormMode::Lufs && peak > 1e-9) {
        const double lufs = measure_lufs(mixL, sr);
        gain = std::pow(10.0, (opt.lufs_target - lufs) / 20.0);
        gain = std::clamp(gain, 0.05, 20.0);
    } else if (opt.normalize == NormMode::Peak && peak > 1e-6) {
        const double target = std::clamp(opt.normalize_target, 0.05, 0.999);
        if (target / peak < 1.0) gain = target / peak;   // 只缩不放
    }
    if (gain != 1.0) {
        const float g = static_cast<float>(gain);
        for (auto& v : mixL) v *= g;
        for (auto& v : mixR) v *= g;
    }

    // ── 真峰值保护 (软限幅, 只在超过 0.98 时起作用) ──────────────
    if (opt.limit) {
        auto soft = [](float& v) {
            const double a = std::fabs(static_cast<double>(v));
            if (a <= 0.98) return;
            const double y = 0.98 + 0.02 * std::tanh((a - 0.98) / 0.02);
            v = static_cast<float>(v < 0 ? -y : y);
        };
        for (auto& v : mixL) soft(v);
        for (auto& v : mixR) soft(v);
    }

    // ── 体检指标 ─────────────────────────────────────────────────
    if (stats) {
        stats->instrument = acoustic.name;
        stats->duration_sec = static_cast<double>(total) / sr;
        stats->total_samples = total;
        stats->chords = chord_blocks;
        stats->max_polyphony = max_poly;
        stats->freq_min = fmin;
        stats->freq_max = fmax;
        stats->warnings = doc.warnings;

        double acc2 = 0.0, dcacc = 0.0;
        size_t silent = 0, clipped = 0;
        stats->max_jump = 0.0;
        for (int ch = 0; ch < (stereo ? 2 : 1); ++ch) {
            const std::vector<float>& src = (ch == 0) ? mixL : mixR;
            double prev = 0.0;
            for (size_t i = 0; i < src.size(); ++i) {
                const double v = src[i];
                stats->peak = std::max(stats->peak, std::fabs(v));
                if (ch == 0) {
                    acc2 += v * v; dcacc += v;
                    if (std::fabs(v) < 1e-4) ++silent;
                    if (std::fabs(v) >= 0.999) ++clipped;
                }
                if (i > 0) {
                    const double j = std::fabs(v - prev);
                    if (j > stats->max_jump) {
                        stats->max_jump = j;
                        stats->max_jump_at = static_cast<double>(i) / sr;
                    }
                }
                prev = v;
            }
            // 咔哒检测: 硬切/不连续会在带限信号里留下"孤立的单样本尖峰"。
            // 判据 = |Δx| 与该样本附近 ±1 ms 平均 |Δx| 的比值:
            // 正常带限波形的比值是个位数, 硬切会高出一到两个数量级。
            const long w = std::max<long>(8, static_cast<long>(0.001 * sr));
            const size_t nb = src.size() / static_cast<size_t>(w) + 1;
            std::vector<double> bsum(nb, 0.0);
            std::vector<double> dd(src.size(), 0.0);
            for (size_t i = 1; i < src.size(); ++i) {
                const double j = std::fabs(static_cast<double>(src[i]) - src[i - 1]);
                dd[i] = j;
                bsum[i / static_cast<size_t>(w)] += j;
            }
            for (size_t i = 1; i < src.size(); ++i) {
                const size_t b = i / static_cast<size_t>(w);
                const double lo = bsum[b] / static_cast<double>(w);
                const double hi = (b + 1 < nb) ? bsum[b + 1] / static_cast<double>(w) : 0.0;
                const double loc = std::max(1e-6, 0.5 * (lo + hi));
                const double ratio = dd[i] / loc;
                if (ratio > stats->click_ratio) {
                    stats->click_ratio = ratio;
                    stats->click_at = static_cast<double>(i) / sr;
                }
            }
        }
        const double nn = static_cast<double>(total ? total : 1);
        stats->rms = std::sqrt(acc2 / nn);
        stats->dc_offset = dcacc / nn;
        stats->crest_db = stats->rms > 1e-12 ? 20.0 * std::log10(stats->peak / stats->rms) : 0.0;
        stats->silence_ratio = static_cast<double>(silent) / nn;
        stats->clipped_ratio = static_cast<double>(clipped) / nn;
        stats->true_peak = std::max(true_peak_of(mixL), stereo ? true_peak_of(mixR) : 0.0);
        stats->lufs = measure_lufs(mixL, sr);

        for (const auto& n : stats->notes) {
            if (n.rest || n.frequency <= 0.0) continue;
            if (n.truncated) ++stats->truncated_notes;
            stats->partials_min = (stats->partials_min == 0) ? n.partials
                                                             : std::min(stats->partials_min, n.partials);
            stats->partials_max = std::max(stats->partials_max, n.partials);
            stats->ring_min_sec = (stats->ring_min_sec == 0.0) ? n.ring_sec
                                                               : std::min(stats->ring_min_sec, n.ring_sec);
            stats->ring_max_sec = std::max(stats->ring_max_sec, n.ring_sec);
            const double cr = n.meas_centroid_hz / n.frequency;
            if (stats->centroid_ratio_min == 0.0) {
                stats->centroid_ratio_min = cr;
                stats->centroid_ratio_max = cr;
            } else {
                stats->centroid_ratio_min = std::min(stats->centroid_ratio_min, cr);
                stats->centroid_ratio_max = std::max(stats->centroid_ratio_max, cr);
            }
            if (n.t30_sec > 0.0) {
                stats->t30_min_sec = (stats->t30_min_sec == 0.0) ? n.t30_sec
                                                                 : std::min(stats->t30_min_sec, n.t30_sec);
                stats->t30_max_sec = std::max(stats->t30_max_sec, n.t30_sec);
            }
        }

        if (doc.meter_num > 0 && doc.meter_den > 0) {
            stats->bar_sec = static_cast<double>(doc.meter_num) * 4.0 / doc.meter_den * beat;
            stats->bars = cursor / stats->bar_sec;
            stats->bar_fit_error = cursor - std::round(stats->bars) * stats->bar_sec;
        }
    }
    // ── 交错输出 ────────────────────────────────────────────────
    std::vector<float> audio;
    if (stereo) {
        audio.resize(total * 2);
        for (size_t i = 0; i < total; ++i) {
            audio[i * 2]     = mixL[i];
            audio[i * 2 + 1] = mixR[i];
        }
    } else {
        audio = std::move(mixL);
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

// ── 旧接口 ─────────────────────────────────────────────────────────
void add_gap(std::vector<float>& samples, double gap_sec, int sample_rate)
{
    int gap_samples = static_cast<int>(sample_rate * gap_sec);
    if (gap_samples <= 0) return;
    samples.resize(samples.size() + static_cast<size_t>(gap_samples), 0.0f);
}
