// 校验 core/ai_prompt: 提示词里的"完整示例"必须能被解析器接受、小节对齐、力度有起伏
#include "ai_prompt.h"
#include "note_parser.h"
#include "tone_gen.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

int main()
{
    const std::string_view example = score_example_rcp();
    if (example.empty()) {
        std::printf("FAIL: score_example_rcp() 为空 —— 提示词正文与示例常量不同步\n");
        return 1;
    }

    std::printf("=== 提示词中的示例 (%zu 字节) ===\n%.*s\n",
                example.size(), (int)example.size(), example.data());

    try {
        RcpDocument doc = parse_rcp(example);
        std::printf("解析: 事件 %zu  拍号 %d/%d  乐器 %s\n",
                    doc.notes.size(), doc.meter_num, doc.meter_den,
                    doc.acoustic.name.c_str());
        for (const auto& w : doc.warnings) std::printf("  警告: %s\n", w.c_str());

        RenderOptions o;
        o.sample_rate = 8000;
        o.stereo = false;
        o.reverb_wet = 0;
        RenderStats st;
        auto audio = render_document(doc, o, &st);

        double vmin = 1.0, vmax = 0.0;
        for (const auto& n : st.notes) {
            if (n.rest) continue;
            vmin = std::min(vmin, n.velocity);
            vmax = std::max(vmax, n.velocity);
        }
        int chords = 0;
        for (size_t i = 0; i < doc.notes.size(); ++i)
            if (doc.notes[i].token_start &&
                (i + 1 >= doc.notes.size() || doc.notes[i + 1].token_start == false))
                ;   // 仅统计用, 见下方 chord 块
        chords = st.chords;

        std::printf("渲染: %.3fs  峰值 %.3f  RMS %.3f  直流 %.2e  丢弃分音 %d\n",
                    st.duration_sec, st.peak, st.rms, st.dc_offset, st.discarded_total);
        std::printf("小节: %.3f 行  对齐偏差 %.4f s  -> %s\n",
                    st.bars, st.bar_fit_error,
                    std::abs(st.bar_fit_error) < 0.02 ? "对齐 OK" : "**不对齐**");
        std::printf("力度: min %.2f / max %.2f  和弦块 %d\n", vmin, vmax, chords);

        const bool ok = std::abs(st.bar_fit_error) < 0.02 && doc.warnings.empty() &&
                        st.dc_offset < 1e-4 && !audio.empty() &&
                        st.clipped_ratio < 1e-6 && vmax > vmin && chords > 0;
        std::printf("\n结论: %s\n", ok ? "提示词示例完全合法 ✅" : "**示例仍有问题 ❌**");
        return ok ? 0 : 2;
    } catch (const std::exception& e) {
        std::printf("FAIL: 解析抛出异常: %s\n", e.what());
        return 1;
    }
}
