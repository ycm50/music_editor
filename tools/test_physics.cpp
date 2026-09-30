// 物理模型自检: 断言本次改动的关键物理行为 (不再是"能解析"而是"物理对不对")
#include "note_parser.h"
#include "tone_gen.h"
#include "note_builder.h"
#include "wav_writer.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
static void check(bool ok, const char* what, const std::string& detail = {})
{
    std::printf("  [%s] %s%s\n", ok ? " OK " : "FAIL", what,
                detail.empty() ? "" : ("  — " + detail).c_str());
    if (!ok) ++g_fail;
}

static double tone_peak(const std::vector<float>& v)
{
    double p = 0.0;
    for (float x : v) p = std::max(p, static_cast<double>(std::fabs(x)));
    return p;
}

int main()
{
    std::printf("== 物理模型自检 ==\n");

    // ① 分音数随音高伸缩: 低音分音多, 高音分音少 (旧模型恒为 12)
    {
        const AcousticParams p = Instruments::piano();
        const auto lo = build_partials(65.41, p, 44100, 1.0);     // C2
        const auto hi = build_partials(1046.5, p, 44100, 1.0);    // C6
        char buf[160];
        std::snprintf(buf, sizeof(buf), "C2=%zu, C6=%zu", lo.size(), hi.size());
        check(lo.size() > hi.size() * 2 && lo.size() >= 40, "低音区分音数远多于高音区", buf);
    }

    // ② 频谱重心/基频随音区变化 (有共鸣体 → 不再是常数)
    {
        const AcousticParams p = Instruments::piano();
        auto ratio = [&](double f0) {
            auto ps = build_partials(f0, p, 44100, 1.0);
            double num = 0, den = 0;
            for (auto& q : ps) { num += q.amp * q.freq; den += q.amp; }
            return den > 0 ? num / den / f0 : 0.0;
        };
        const double a = ratio(130.81), b = ratio(1046.5);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "C3 比=%.2f, C6 比=%.2f", a, b);
        check(std::fabs(a - b) > 0.2, "重心/基频随音区明显变化", buf);
    }

    // ③ 力度耦合: 重弹更亮 (重心上移)
    {
        const AcousticParams p = Instruments::piano();
        auto centroid = [&](double v) {
            auto ps = build_partials(261.63, p, 44100, v);
            double num = 0, den = 0;
            for (auto& q : ps) { num += q.amp * q.freq; den += q.amp; }
            return den > 0 ? num / den : 0.0;
        };
        const double pp = centroid(0.15), ff = centroid(1.0);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "pp=%.0f Hz, ff=%.0f Hz", pp, ff);
        check(ff > pp * 1.15, "力度改变音色 (ff 比 pp 亮 >15%)", buf);
    }

    // ④ 制音器/自然余音: 钟写 1 拍也要响很久; 钢琴松键后很快收
    {
        std::vector<TonePartial> dummy;
        (void)dummy;
        AcousticParams bell = Instruments::bells();
        AcousticParams pno  = Instruments::piano();
        auto tb = generate_tone(261.63, 0.5, {}, 44100, bell, {}, 1.0);
        auto tp = generate_tone(261.63, 0.5, {}, 44100, pno, {}, 1.0);
        const double bell_s = double(tb.size()) / 44100.0;
        const double pno_s  = double(tp.size()) / 44100.0;
        char buf[200];
        std::snprintf(buf, sizeof(buf), "钟=%.2fs, 钢琴=%.2fs", bell_s, pno_s);
        check(bell_s > 5.0, "无制音器乐器(钟)自然余音 (>5s)", buf);
        check(pno_s > 0.5 && pno_s < 1.5, "有制音器乐器(钢琴)松键后按齿毡衰减", buf);
    }

    // ⑤ 音块首尾必须淡到 0 (硬切是咔哒的唯一来源)
    {
        bool ok = true;
        std::string worst;
        for (const auto& p : Instruments::all()) {
            auto t = generate_tone(440.0, 0.4, {}, 44100, p, {}, 0.9);
            if (t.empty()) { ok = false; worst = p.name + " 空"; continue; }
            const double head = std::fabs(t.front());
            const double tail = std::fabs(t.back());
            if (head > 1e-3 || tail > 1e-3) {
                ok = false;
                char buf[160];
                std::snprintf(buf, sizeof(buf), "%s head=%.4f tail=%.4f", p.name.c_str(), head, tail);
                worst = buf;
                break;
            }
        }
        check(ok, "全部 33 种乐器的音块首尾都淡到 0 (无硬切)", worst);
    }

    // ⑥ 输出电平在 [-1,1] 内且无直流
    {
        RcpDocument doc = parse_rcp("120,261.63,0.5\n@timbre organ\n1.02 3.02 5.02\n");
        RenderOptions opt;
        opt.stereo = true;
        RenderStats st;
        auto audio = render_document(doc, opt, &st);
        char buf[200];
        std::snprintf(buf, sizeof(buf), "peak=%.3f dc=%.2e jump=%.3f lufs=%.1f",
                      st.peak, st.dc_offset, st.max_jump, st.lufs);
        check(st.peak <= 1.0 && std::fabs(st.dc_offset) < 1e-4 && st.boundary_glitches == 0,
              "整曲渲染: 无削波/无直流/无硬切", buf);
    }

    // ⑦ 立体声声像: 低音偏左, 高音偏右 (钢琴键盘感)
    {
        RcpDocument doc = parse_rcp("120,261.63,0.5\n@timbre piano\n1.-2 1.+2\n");
        RenderOptions opt;
        opt.stereo = true;
        opt.reverb_wet = 0.0;
        auto audio = render_document(doc, opt, nullptr);
        double lo_l = 0, lo_r = 0, hi_l = 0, hi_r = 0;
        const size_t n = audio.size() / 2;
        for (size_t i = 0; i < n / 2; ++i)      { lo_l += std::fabs(audio[i*2]); lo_r += std::fabs(audio[i*2+1]); }
        for (size_t i = n / 2; i < n; ++i)      { hi_l += std::fabs(audio[i*2]); hi_r += std::fabs(audio[i*2+1]); }
        char buf[200];
        std::snprintf(buf, sizeof(buf), "低音 L/R=%.2f, 高音 L/R=%.2f",
                      lo_l / std::max(1e-9, lo_r), hi_l / std::max(1e-9, hi_r));
        check(lo_l > lo_r && hi_r > hi_l, "声像随音高展开: 低音偏左, 高音偏右", buf);
    }

    // ⑧ 音块构造器 (插入面板) 生成的文本都能被解析
    {
        bool ok = true;
        std::string bad;
        for (int deg = 0; deg <= 7; ++deg) {
            for (int den : {1, 2, 4, 8, 16, 32}) {
                NoteSpec s;
                s.degree = deg;
                s.denominator = den;
                s.dotted = (den == 4);
                s.tuplet = (den == 8) ? 3 : 0;
                s.accidental = deg % 3;
                s.octave = (deg % 5) - 2;
                s.articulation = deg % 3;
                s.ov_pos = true; s.pos = 0.125;
                s.ov_damper = true; s.damper = 0.08;
                NoteSpec m;
                m.degree = 3; m.denominator = 4; m.octave = 1;
                s.chord_tokens.push_back(render_note_core(m));
                const std::string tok = render_note_token(s);
                const std::string doc = "120,261.63,0.5\n@timbre piano\n" + tok + "\n";
                try {
                    RcpDocument d = parse_rcp_strict(doc);
                    if (d.notes.empty()) { ok = false; bad = tok + " (无音符)"; break; }
                } catch (const std::exception& e) {
                    ok = false; bad = tok + " → " + e.what(); break;
                }
            }
            if (!ok) break;
        }
        check(ok, "插入面板生成的 token 全部可被严格解析", bad);
    }

    // ⑨ @acoustic 行往返: 生成的指令能被解析回同样的物理量
    {
        bool ok = true;
        std::string bad;
        for (const auto& p : Instruments::all()) {
            const std::string line = render_acoustic_directive(p);
            const std::string doc = "120,261.63,0.5\n" + line + "\n1.04\n";
            try {
                RcpDocument d = parse_rcp_strict(doc);
                // 非谐性 B 与 τ₁ 必须一致 (取相同值时)
                if (std::fabs(d.acoustic.inharmonicity - p.inharmonicity)
                        > 1e-4 * std::max(1e-9, std::fabs(p.inharmonicity)) + 1e-9) {
                    ok = false; bad = p.name + " B 不一致"; break;
                }
                if (std::fabs(d.acoustic.decay - p.decay) > 1e-3 * std::max(0.01, p.decay)) {
                    ok = false; bad = p.name + " τ₁ 不一致"; break;
                }
                if (std::fabs(d.acoustic.damper - p.damper) > 1e-6
                    || std::fabs(d.acoustic.strike_pos - p.strike_pos) > 1e-6
                    || d.acoustic.unison != p.unison
                    || d.acoustic.body.size() != p.body.size()
                    || d.acoustic.partial_table.size() != p.partial_table.size()) {
                    ok = false; bad = p.name + " 参数往返不一致"; break;
                }
            } catch (const std::exception& e) {
                ok = false; bad = p.name + " → " + e.what(); break;
            }
        }
        check(ok, "@acoustic 指令往返一致 (33 种预设)", bad);
    }

    // ⑩ 位深与 dither: 16/24-bit WAV 头合法, 采样值不越界
    {
        std::vector<float> s(1000);
        for (size_t i = 0; i < s.size(); ++i)
            s[i] = static_cast<float>(0.9 * std::sin(2.0 * 3.14159265 * 440.0 * i / 44100.0));
        auto p16 = to_pcm16(s, true);
        auto p24 = to_pcm24(s, false);
        auto w16 = encode_wav(p16, 44100, 2);
        auto w24 = encode_wav24(p24, 44100, 2);
        const bool ok = w16.size() == 44 + p16.size() * 2
                        && w24.size() == 44 + p24.size()
                        && w24[34] == 24 && w16[34] == 16;
        char buf[160];
        std::snprintf(buf, sizeof(buf), "16-bit=%zu B, 24-bit=%zu B, bits=%u/%u",
                      w16.size(), w24.size(), w16[34], w24[34]);
        check(ok, "16/24-bit WAV 编码正确 (头 + 数据长度)", buf);
    }

    std::printf("\n%s (失败 %d 项)\n", g_fail ? "**物理自检失败**" : "物理自检全部通过 ✅", g_fail);
    return g_fail ? 1 : 0;
}
