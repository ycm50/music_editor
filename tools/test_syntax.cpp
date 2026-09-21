// 语法单元测试: 每种写法都必须能正确解析 (含提示词中用到的写法)
#include "note_parser.h"
#include <cstdio>
#include <string>

static int g_fail = 0;

static void dump(const char* label, const char* content)
{
    std::printf("[%s]\n", label);
    try {
        RcpDocument d = parse_rcp(content);
        for (size_t i = 0; i < d.notes.size() && i < 8; ++i)
            std::printf("    %8.2fHz %6.3fs vel=%.2f%s\n", d.notes[i].frequency,
                        d.notes[i].duration_sec, d.notes[i].velocity,
                        d.notes[i].rest ? "  REST" : "");
        for (auto& w : d.warnings) std::printf("    warn: %s\n", w.c_str());
        if (d.notes.empty()) { std::printf("    ** 无音符 **\n"); ++g_fail; }
    } catch (const std::exception& e) {
        std::printf("    THROW: %s\n", e.what());
        ++g_fail;
    }
}

// 断言: 力度为 expect_vel 的事件恰好有 n 个, 且其余事件力度为 other_vel
static void expect_group_vel(const char* label, const char* content, int n,
                             double expect_vel, double other_vel)
{
    std::printf("[%s] ", label);
    try {
        RcpDocument d = parse_rcp(content);
        int hit = 0, miss = 0;
        for (const auto& note : d.notes) {
            if (std::abs(note.velocity - expect_vel) < 1e-9) ++hit;
            else if (std::abs(note.velocity - other_vel) < 1e-9) ++miss;
            else { hit = -999; break; }
        }
        const bool ok = (hit == n) && (miss >= 0);
        std::printf("%s (%zu 事件: %.2f 的有 %d 个, 期望 %d 个)\n",
                    ok ? "OK" : "**FAIL**", d.notes.size(), expect_vel, hit, n);
        if (!ok) ++g_fail;
    } catch (const std::exception& e) {
        std::printf("**THROW** %s\n", e.what());
        ++g_fail;
    }
}

int main()
{
    dump("力度 pp/p/mf/ff/数值",
         "120,261.63,0.5\n@timbre piano\n1.04!p 2.04!mf 3.04!ff 4.04!0.33 5.04\n");
    dump("多声部", "120,261.63,0.5\n@timbre piano\n@voice 1\n1.04 2.04\n@voice 2\n1.-04 5.-04\n");
    dump("连音+和弦", "120,261.63,0.5\n@timbre piano\n[1.-02,3.-02]_[1.-02,3.-02]\n");
    dump("变音号前置", "120,261.63,0.5\n@timbre piano\n#4.04 b7.04 b3.04 ##1.04\n");
    dump("附点/三连音", "120,261.63,0.5\n@timbre piano\n1.04t 1.04t 1.04t 2.08. 2.08.\n");
    dump("休止与注释", "120,261.63,0.5\n@timbre piano\n1.04  # 注释\n # 整行注释\nr.04 0.04\n");
    dump("音名参考", "@ref A4=440\n@bpm 100\n@beat 0.6\n@meter 3/4\n@timbre harp\n1.04 2.04 3.04\n");
    dump("和弦逗号分隔", "96,261.63,0.625\n@timbre piano\n[1.04,3.04,5.04]\n");
    dump("和弦+分隔与八度+",
         "96,261.63,0.625\n@timbre piano\n[6.04+1.+04+3.+04]\n");

    // 修饰符作用于整个和弦: [!f 的和弦] 3 个成员应为 1.00, 后面的独立音符保持 1.00 默认
    expect_group_vel("和弦后接 !f (3 成员=0.78, 其余默认 1.0)",
                     "96,261.63,0.625\n@timbre piano\n[1.02,3.02,5.02]!f 5.04 4.04\n", 3, 0.78, 1.0);
    expect_group_vel("和弦后接 !pp (3 成员=0.15)",
                     "96,261.63,0.625\n@timbre piano\n[1.02,3.02,5.02]!pp\n", 3, 0.15, -1.0);
    expect_group_vel("和弦带八度+后接 !mf (3 成员=0.60)",
                     "96,261.63,0.625\n@timbre piano\n[6.02,1.+02,3.+02]!mf\n", 3, 0.60, -1.0);
    expect_group_vel("连音和弦后接 !f (2 成员=1.0)",
                     "96,261.63,0.625\n@timbre piano\n[1.02,3.02]_[1.02,3.02]!f\n", 2, 1.0, -1.0);
    expect_group_vel("和弦内逐音 $dec 不影响力度 (2 成员=1.0)",
                     "96,261.63,0.625\n@timbre piano\n[1.04,3.04]$dec=0.3\n", 2, 1.0, -1.0);
    // 对比: 和弦 !f + 后续音符 !p, 两者必须互不干扰
    expect_group_vel("和弦 !f 与后面音符 !p 互不影响",
                     "96,261.63,0.625\n@timbre piano\n[1.02,3.02,5.02]!f 5.04!p 4.04!p\n", 3, 0.78, 0.30);

    // 旧式写法必须继续可用 (向后兼容)
    dump("旧式三相头+谐波表+持续比例行",
         "120,261.63,0.5\n"
         "1,0.7,0.5,0.3,0.2,0.15,0.1,0.08,0.05,0.03\n"
         "0-1!0-0.8!0-0.6!{0-0.4,0.5-0.7>}!0-0.5!0-0.3!0-0.2!0-0.1!0-0.1!0-0.1\n"
         "1.04 3.04 5.04\n");
    dump("带小数振幅的旧式音色行", "120,261.63,0.5\n1,0.7,0.5,0.3\n1.04 3.04\n");

    // 容错: 一个坏 token 只应被跳过, 其余音符照常 (不应整体解析失败)
    {
        std::printf("[容错: 坏 token 只跳过, 不整体失败] ");
        try {
            RcpDocument d = parse_rcp("120,261.63,0.5\n@timbre piano\n1.04 5.00:5 3.04\n");
            const bool ok = d.notes.size() == 2 && !d.warnings.empty();
            std::printf("%s (保留 %zu 个音符, %zu 条提示)\n",
                        ok ? "OK" : "**FAIL**", d.notes.size(), d.warnings.size());
            if (!ok) ++g_fail;
        } catch (const std::exception& e) {
            std::printf("**THROW** %s\n", e.what());
            ++g_fail;
        }
    }
    // 严格模式: 同一个坏 token 必须抛错且带行号
    {
        std::printf("[严格模式: 坏 token 抛错并带行号] ");
        try {
            (void)parse_rcp_strict("120,261.63,0.5\n@timbre piano\n1.04 5.00:5 3.04\n");
            std::printf("**FAIL** (未抛错)\n");
            ++g_fail;
        } catch (const std::exception& e) {
            const std::string msg = e.what();
            const bool ok = msg.find("第 3 行") != std::string::npos;
            std::printf("%s (%s)\n", ok ? "OK" : "**FAIL**", msg.c_str());
            if (!ok) ++g_fail;
        }
    }

    std::printf("\n%s (失败 %d 项)\n", g_fail ? "**有用例失败**" : "全部用例通过 ✅", g_fail);
    return g_fail ? 1 : 0;
}
