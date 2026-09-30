#include "note_builder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace {

std::string num(double v, int prec = 6)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.*g", prec, v);
    return buf;
}

/// 音级 + 变音 + 八度 + 拍长分母 (含附点/连音), 不含力度等修饰
std::string core_note(const NoteSpec& s)
{
    std::string out;
    if (s.degree <= 0) {
        out = "0";
    } else {
        const int deg = std::clamp(s.degree, 1, 7);
        if (s.accidental > 0)
            out.append(static_cast<size_t>(std::clamp(s.accidental, 0, 2)), '#');
        else if (s.accidental < 0)
            out.append(static_cast<size_t>(std::clamp(-s.accidental, 0, 2)), 'b');
        out.push_back(static_cast<char>('0' + deg));
    }
    out.push_back('.');
    const int oct = std::clamp(s.octave, -4, 4);
    if (oct == 0) out.push_back('0');
    else out.append(static_cast<size_t>(std::abs(oct)), oct > 0 ? '+' : '-');

    static const int kValid[] = {1, 2, 4, 8, 16, 32};
    int den = 4;
    for (int v : kValid) if (v == s.denominator) den = v;
    out += std::to_string(den);
    if (s.dotted) out.push_back('.');
    if (s.tuplet == 3) out.push_back('t');
    else if (s.tuplet == 5) out.push_back('q');
    return out;
}

/// 修饰符: !力度 ~ ' $覆盖
std::string modifiers(const NoteSpec& s)
{
    std::string out;
    if (s.articulation == 1) out.push_back('~');
    else if (s.articulation == 2) out.push_back('\'');
    if (s.use_velocity) {
        const auto& names = velocity_names();
        if (s.velocity_id >= 0 && s.velocity_id < static_cast<int>(names.size()))
            out += "!" + names[static_cast<size_t>(s.velocity_id)];
        else
            out += "!" + num(std::clamp(s.velocity, 0.0, 4.0), 4);
    }
    std::vector<std::string> ov;
    if (s.ov_atk)    ov.push_back("atk=" + num(s.atk, 4));
    if (s.ov_dec)    ov.push_back("dec=" + num(s.dec, 4));
    if (s.ov_sus)    ov.push_back("sus=" + num(s.sus, 4));
    if (s.ov_br)     ov.push_back("br=" + num(s.br, 4));
    if (s.ov_B)      ov.push_back("b=" + num(s.B, 6));
    if (s.ov_pos)    ov.push_back("pos=" + num(s.pos, 4));
    if (s.ov_damper) ov.push_back("damper=" + num(s.damper, 4));
    if (!ov.empty()) {
        out.push_back('$');
        for (size_t i = 0; i < ov.size(); ++i) {
            if (i) out.push_back('/');
            out += ov[i];
        }
    }
    return out;
}

} // namespace

std::string render_note_core(const NoteSpec& spec)
{
    return core_note(spec);
}

std::string render_note_token(const NoteSpec& spec)
{
    NoteSpec s = spec;
    s.repeat = std::clamp(s.repeat, 1, 64);

    std::string token;
    if (s.tie) token.push_back('_');

    if (s.chord.empty() && s.chord_tokens.empty()) {
        token += core_note(s);
    } else {
        token.push_back('[');
        token += core_note(s);
        for (const auto& raw : s.chord_tokens) {
            token.push_back(',');
            token += raw;
        }
        for (const auto& m : s.chord) {
            NoteSpec mm = m;
            mm.use_velocity = false;      // 和弦成员不重复写力度
            mm.articulation = 0;
            mm.tie = false;
            mm.chord.clear();
            token.push_back(',');
            token += core_note(mm);
        }
        token.push_back(']');
    }
    token += modifiers(s);

    std::string out;
    std::string prefix;
    if (s.voice > 0) prefix = "@voice " + std::to_string(s.voice) + "\n";
    for (int i = 0; i < s.repeat; ++i) {
        if (i) out.push_back(' ');
        out += token;
    }
    return prefix + out;
}

std::string render_timbre_directive(std::string_view instrument)
{
    return "@timbre " + std::string(instrument);
}

std::string render_acoustic_directive(const AcousticParams& p)
{
    const AcousticParams d{};
    std::vector<std::string> kv;
    if (p.inharmonicity != d.inharmonicity) kv.push_back("B=" + num(p.inharmonicity));
    if (p.inharm_alpha != d.inharm_alpha)   kv.push_back("alpha=" + num(p.inharm_alpha));
    if (p.tilt_db_oct != d.tilt_db_oct)     kv.push_back("tilt=" + num(p.tilt_db_oct));
    if (p.decay != d.decay)                 kv.push_back("damp=" + num(p.decay));
    if (p.damping_law == DampingLaw::TwoTerm && p.damping_n2 != d.damping_n2)
        kv.push_back("n2=" + num(p.damping_n2));
    else if (p.damping_law == DampingLaw::PowerExp && p.damping_exp != d.damping_exp)
        kv.push_back("damp=" + num(p.decay) + "/" + num(p.damping_exp));
    if (p.attack != d.attack)               kv.push_back("atk=" + num(p.attack));
    if (p.sustain != d.sustain)             kv.push_back("sus=" + num(p.sustain));
    if (p.damper != d.damper)               kv.push_back("damper=" + num(p.damper));
    if (p.ring != d.ring)                   kv.push_back("ring=" + num(p.ring));
    if (p.noise != d.noise)                 kv.push_back("noise=" + num(p.noise));
    if (p.noise_len != d.noise_len)         kv.push_back("nl=" + num(p.noise_len));
    if (p.strike_pos != d.strike_pos)       kv.push_back("pos=" + num(p.strike_pos));
    if (p.strike_p != d.strike_p)           kv.push_back("posp=" + num(p.strike_p));
    if (p.vel_tilt != d.vel_tilt)           kv.push_back("vtilt=" + num(p.vel_tilt));
    if (p.vel_attack != d.vel_attack)       kv.push_back("velatk=" + num(p.vel_attack));
    if (p.vel_noise != d.vel_noise)         kv.push_back("velnoise=" + num(p.vel_noise));
    if (p.decay2_beta != d.decay2_beta)     kv.push_back("beta=" + num(p.decay2_beta));
    if (p.unison != d.unison)               kv.push_back("uni=" + std::to_string(p.unison));
    if (p.detune_cents != d.detune_cents)   kv.push_back("detune=" + num(p.detune_cents));
    if (p.stereo != d.stereo)               kv.push_back("stereo=" + num(p.stereo));
    if (p.pan != d.pan)                     kv.push_back("pan=" + num(p.pan));
    if (p.pan_pitch != d.pan_pitch)         kv.push_back("panpitch=" + num(p.pan_pitch));
    if (p.partial_floor != d.partial_floor) kv.push_back("floor=" + num(p.partial_floor));
    if (p.max_partials != d.max_partials)   kv.push_back("maxp=" + std::to_string(p.max_partials));
    if (p.stretch != d.stretch)             kv.push_back("stretch=" + num(p.stretch));
    if (p.phase == PhaseMode::Random)       kv.push_back("phase=random");
    if (p.temperament == Temperament::Just) kv.push_back("temp=just");
    else if (p.temperament == Temperament::Pythagorean) kv.push_back("temp=pyth");
    if (p.body_hp_hz > 0.0)
        kv.push_back("hpf=" + num(p.body_hp_hz) + ":" + num(p.body_hp_db_oct));
    if (!p.body.empty()) {
        std::string b;
        for (size_t i = 0; i < p.body.size(); ++i) {
            if (i) b.push_back(',');
            b += num(p.body[i].freq_hz) + ":" + num(p.body[i].q) + ":" + num(p.body[i].gain_db);
        }
        kv.push_back("body=" + b);
    }
    if (!p.partial_table.empty()) {
        std::string t;
        for (size_t i = 0; i < p.partial_table.size(); ++i) {
            if (i) t.push_back(',');
            t += num(p.partial_table[i].ratio) + ":" + num(p.partial_table[i].amp)
                 + ":" + num(p.partial_table[i].tau);
        }
        kv.push_back("table=" + t);
    }
    if (!p.partial_decay.empty()) {
        std::string t;
        for (size_t i = 0; i < p.partial_decay.size(); ++i) {
            if (i) t.push_back(',');
            t += num(p.partial_decay[i]);
        }
        kv.push_back("damp=" + t);
    }

    std::string line = "@acoustic " + (p.name.empty() ? std::string("custom") : p.name);
    for (size_t i = 0; i < kv.size(); ++i) {
        line += (i == 0 ? " " : ";");
        line += kv[i];
    }
    return line;
}

const std::vector<std::string>& velocity_names()
{
    return dynamic_names();
}

std::vector<InstrumentInfo> instrument_catalog()
{
    static const std::vector<InstrumentInfo> list = [] {
        std::vector<InstrumentInfo> v;
        for (const auto& p : Instruments::all()) {
            InstrumentInfo info;
            info.name = p.name;
            info.category = Instruments::category_of(p.name);
            info.desc = "τ₁=" + num(p.decay, 3) + "s";
            if (p.damper > 0.0) info.desc += " 制音" + num(p.damper, 2) + "s";
            else                info.desc += " 自由余音";
            if (p.sustain > 0.3) info.desc += " 持续型";
            if (p.unison > 1)    info.desc += " " + std::to_string(p.unison) + "弦组";
            if (!p.partial_table.empty()) info.desc += " 显式分音";
            if (p.tilt_db_oct > 0.0) info.desc += " 倾斜" + num(p.tilt_db_oct, 3) + "dB/oct";
            v.push_back(std::move(info));
        }
        return v;
    }();
    return list;
}
