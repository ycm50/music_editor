/**
 * save — RCP → WAV 转换器 / 乐谱体检工具
 *
 * 用法:
 *   save <file.rcp> [选项]
 *
 * 选项:
 *   -t, --timbre NAME      乐器/音色 (文件内 @timbre 优先)
 *   -o, --output FILE      输出 WAV 路径
 *       --mono             单声道输出 (默认立体声 + 混响)
 *       --no-reverb        关闭混响
 *       --sample-rate N    采样率 (默认 44100)
 *       --duration-comp    短音能量补偿 (等响)
 *       --analyze          输出乐理/声学体检报告
 *       --notes N          报告中逐音明细条数 (默认 0 = 只给汇总)
 *       --list             列出可用乐器与音色
 *
 * 依赖: music-core (无需 Qt)
 */

#include "note_parser.h"
#include "tone_gen.h"
#include "wav_writer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ── 读取文件 ───────────────────────────────────────────────────────
static std::string read_file(const std::string& path)
{
    if (path == "-") {
        std::istreambuf_iterator<char> begin(std::cin), end;
        return {begin, end};
    }
    std::ifstream ifs(path);
    if (!ifs.is_open())
        throw std::runtime_error("Cannot open file: " + path);
    std::istreambuf_iterator<char> begin(ifs), end;
    return {begin, end};
}

// ── 音高名 (报告用) ───────────────────────────────────────────────
static std::string pitch_name(double freq)
{
    if (freq <= 0.0) return "rest";
    static const char* names[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    double midi = 69.0 + 12.0 * std::log2(freq / 440.0);
    int m = static_cast<int>(std::lround(midi));
    int pc = ((m % 12) + 12) % 12;
    int oct = m / 12 - 1;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s%d", names[pc], oct);
    return buf;
}

// ── 音色/乐器解析: 文件内 @timbre 优先, 否则命令行 ────────────────
static void resolve_timbre(RcpDocument& doc, const std::string& name)
{
    if (doc.has_acoustic || doc.has_harmonics) return;   // 文件已自带, 不覆盖

    std::string lower;
    for (char c : name) lower.push_back(static_cast<char>(std::tolower((unsigned char)c)));

    // 物理乐器优先 (含 piano/violin/flute 的完整物理模型)
    if (const AcousticParams* p = Instruments::find_by_name(lower)) {
        doc.acoustic = *p;
        doc.has_acoustic = true;
        doc.timbre_name = p->name;
        return;
    }
    // 回退: 旧式裸谐波音色
    if (const Timbre* t = Timbres::find_by_name(lower)) {
        doc.harmonics = t->harmonics;
        doc.has_harmonics = true;
        doc.timbre_name = t->name;
        return;
    }
    throw std::runtime_error("Unknown timbre/instrument: " + name +
                             " (用 --list 查看可用列表)");
}

static void print_list()
{
    std::cout << "物理乐器 (@timbre / --timbre, 支持完整物理模型):\n";
    for (const auto& p : Instruments::all()) {
        std::cout << "  " << std::left << std::setw(12) << p.name
                  << " B=" << std::setw(8) << p.inharmonicity
                  << " damp=" << std::setw(5) << p.decay << "s"
                  << " atk=" << std::setw(6) << p.attack << "s"
                  << " sus=" << std::setw(5) << p.sustain
                  << " br=" << std::setw(5) << p.brightness
                  << " noise=" << p.noise << "\n";
    }
    std::cout << "\n旧式裸谐波音色 (只有频谱, 无衰减/非谐性):\n";
    for (const auto& t : Timbres::all())
        std::cout << "  " << t.name << " (" << t.harmonics.size() << " 谐波)\n";
}

// ── 体检报告 ───────────────────────────────────────────────────────
static void print_analysis(const RcpDocument& doc,
                           const RenderStats& st,
                           const std::vector<float>& audio,
                           bool stereo,
                           int sample_rate,
                           int note_detail)
{
    auto line = [] { std::cout << "  " << std::string(66, '-') << "\n"; };

    std::cout << "\n===== 乐理 / 声学体检 =====\n";
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "音色模型   : " << (st.instrument.empty() ? "(裸谐波表)" : st.instrument)
              << (doc.has_harmonics ? " + 显式谐波表" : "") << "\n";
    std::cout << "时长       : " << st.duration_sec << " s @ " << sample_rate << " Hz "
              << (stereo ? "立体声" : "单声道") << "\n";
    std::cout << "音符/休止  : " << st.note_count << " / " << st.rest_count
              << "   (连音已合并, 和弦成员分列)\n";
    std::cout << "和弦块/最大同时发声 : " << st.chords << " / " << st.max_polyphony << "\n";
    if (st.freq_min > 0.0)
        std::cout << "音域       : " << st.freq_min << " Hz (" << pitch_name(st.freq_min)
                  << ") ~ " << st.freq_max << " Hz (" << pitch_name(st.freq_max) << ")\n";

    std::cout << "\n-- 波形指标 --\n";
    std::cout << "峰值 / RMS : " << st.peak << " / " << st.rms
              << "   峰均比 " << st.crest_db << " dB\n";
    std::cout << "直流偏置   : " << std::scientific << st.dc_offset << std::fixed
              << "   (|DC|<1e-4 为合格)\n";
    std::cout << "静音占比   : " << st.silence_ratio * 100.0 << " %\n";
    std::cout << "削波占比   : " << st.clipped_ratio * 100.0 << " %\n";
    std::cout << "混叠分音   : " << st.discarded_total << " 个被丢弃 (Nyquist="
              << sample_rate / 2 << " Hz)\n";

    if (st.bar_sec > 0.0) {
        std::cout << "\n-- 节拍校验 (@meter " << doc.meter_num << "/" << doc.meter_den << ") --\n";
        std::cout << "小节时长   : " << st.bar_sec << " s   总长 " << st.bars << " 小节\n";
        std::cout << "对齐偏差   : " << st.bar_fit_error << " s ("
                  << (std::abs(st.bar_fit_error) < 0.02 ? "对齐" : "与拍号不符") << ")\n";
    }

    if (!st.warnings.empty()) {
        std::cout << "\n-- 解析提示 --\n";
        for (const auto& w : st.warnings) std::cout << "  ! " << w << "\n";
    }

    if (note_detail > 0 && !st.notes.empty()) {
        std::cout << "\n-- 逐音明细 (前 " << std::min<int>(note_detail, (int)st.notes.size())
                  << " 个) --\n";
        std::cout << "  #   频率(Hz)  音名    时长(s)  力度  分音 混叠  重心(Hz)  τ1(s)\n";
        for (int i = 0; i < note_detail && i < (int)st.notes.size(); ++i) {
            const auto& n = st.notes[i];
            std::cout << "  " << std::setw(3) << n.index << "  ";
            if (n.rest) {
                std::cout << "  (休止)                    " << std::setw(6) << n.duration_sec << "\n";
                continue;
            }
            std::cout << std::setw(9) << n.frequency << "  "
                      << std::setw(6) << pitch_name(n.frequency) << "  "
                      << std::setw(7) << n.duration_sec << "  "
                      << std::setw(4) << n.velocity << "  "
                      << std::setw(4) << n.partials << " "
                      << std::setw(4) << n.discarded << "  "
                      << std::setw(8) << n.centroid_hz << "  "
                      << std::setw(6) << n.tau1_sec << "\n";
        }
    }
    line();
    std::cout << "(音频样本 " << audio.size() << " 个 float32, "
              << (audio.size() * 4.0 / 1048576.0) << " MB)\n";
}

// ── 规范化单行指标 (--metrics): 供 CI 跨平台逐字节比对合成一致性 ──
// 只输出对平台无关的确定性数值; 不打印耗时/内存等环境相关量。
static void print_metrics(const std::string& name, const RenderStats& st, int note_count_raw)
{
    std::ostringstream os;
    os << std::fixed << std::setprecision(6)
       << "METRICS " << name
       << " events=" << note_count_raw
       << " samples=" << st.total_samples
       << " dur=" << st.duration_sec
       << " peak=" << st.peak
       << " rms=" << st.rms
       << " dc=" << std::scientific << std::setprecision(3) << st.dc_offset
       << std::fixed << std::setprecision(6)
       << " crest=" << st.crest_db
       << " clip=" << st.clipped_ratio
       << " silent=" << st.silence_ratio
       << " discarded=" << st.discarded_total
       << " chords=" << st.chords
       << " poly=" << st.max_polyphony
       // 对齐偏差 < 1ms 视为 0: 避免 -0.000000 / 0.000000 这种负零在不同
       // 架构/编译器下不一致, 导致 CI 一致性比对假失败
       << " fit=" << (std::abs(st.bar_fit_error) < 1e-3 ? 0.0 : st.bar_fit_error)
       << " fmin=" << st.freq_min
       << " fmax=" << st.freq_max;
    std::cout << os.str() << "\n";
}

// ── 乐谱校验 (--check): 返回 0 通过 / 1 失败 ───────────────────────
// 断言内容与小节严格程度:
//   1) 至少有一个音符
//   2) 音符总长能被 @meter 的小节整除 (偏差 < 20ms)
//   3) 削波占比 < 0.1%
//   4) 直流偏置 < 1e-4
static int run_check(const RcpDocument& doc, const RenderStats& st)
{
    std::vector<std::string> problems;

    if (doc.notes.empty())
        problems.push_back("未解析到任何音符");

    if (st.bar_sec > 0.0 && std::abs(st.bar_fit_error) >= 0.02) {
        std::ostringstream os;
        os << std::fixed << std::setprecision(3)
           << "音符总长与 " << doc.meter_num << "/" << doc.meter_den
           << " 小节不对齐: 共 " << st.bars << " 小节, 偏差 " << st.bar_fit_error << " s";
        problems.push_back(os.str());
    }
    if (st.clipped_ratio >= 1e-3)
        problems.push_back("削波占比过高: " + std::to_string(st.clipped_ratio * 100.0) + "%");
    if (std::abs(st.dc_offset) >= 1e-4)
        problems.push_back("存在直流偏置: " + std::to_string(st.dc_offset));

    std::cout << "校验: " << doc.notes.size() << " 个事件, "
              << std::fixed << std::setprecision(2) << st.duration_sec << " s, "
              << st.bars << " 小节, 峰值 " << st.peak << ", 削波 " << st.clipped_ratio * 100.0
              << "%\n";
    if (problems.empty()) {
        std::cout << "结论: 通过 ✅\n";
        return 0;
    }
    for (const auto& p : problems) std::cerr << "  ✗ " << p << "\n";
    std::cout << "结论: 不通过 ❌\n";
    return 1;
}

// ── 主函数 ─────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    std::string input_file;
    std::string output_file;
    std::string timbre_name = "piano";
    std::string force_timbre;
    bool timbre_given = false;
    bool analyze = false;
    bool list = false;
    bool check = false;
    bool quiet = false;
    bool metrics = false;
    int  note_detail = 0;

    RenderOptions opt;
    opt.stereo = true;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << what << " requires a value\n";
                std::exit(1);
            }
            return argv[++i];
        };
        if (arg == "--timbre" || arg == "-t") { timbre_name = need("--timbre"); timbre_given = true; }
        else if (arg == "--force-timbre" || arg == "-T") { force_timbre = need("--force-timbre"); }
        else if (arg == "--output" || arg == "-o") { output_file = need("--output"); }
        else if (arg == "--sample-rate" || arg == "-r") { opt.sample_rate = std::stoi(need("--sample-rate")); }
        else if (arg == "--notes") { note_detail = std::stoi(need("--notes")); }
        else if (arg == "--analyze" || arg == "-a") { analyze = true; }
        else if (arg == "--check") { check = true; analyze = true; }
        else if (arg == "--quiet" || arg == "-q") { quiet = true; }
        else if (arg == "--metrics") { metrics = true; quiet = true; analyze = true; }
        else if (arg == "--mono") { opt.stereo = false; }
        else if (arg == "--stereo") { opt.stereo = true; }
        else if (arg == "--no-reverb") { opt.reverb_wet = 0.0; }
        else if (arg == "--reverb") { opt.reverb_wet = std::stod(need("--reverb")); }
        else if (arg == "--duration-comp") { opt.dur_compensate = true; }
        else if (arg == "--list") { list = true; }
        else if (arg == "--help" || arg == "-h") {
            std::cout <<
                "用法: save <file.rcp> [选项]\n"
                "  -t, --timbre NAME    乐器/音色 (仅在文件未指定音色时生效)\n"
                "  -T, --force-timbre   强制覆盖文件内音色 (同谱 A/B 对比)\n"
                "  -o, --output FILE    输出 WAV 路径\n"
                "      --mono           单声道 (默认立体声+混响)\n"
                "      --no-reverb      关闭混响\n"
                "      --sample-rate N  采样率 (默认 44100)\n"
                "      --duration-comp  短音能量补偿\n"
                "      --analyze        乐理/声学体检报告\n"
                "      --check          校验乐谱: 严格模式 + 小节对齐/削波/直流断言,\n"
                "                       不写出 WAV, 有问题时以非 0 退出 (供 CI 使用)\n"
                "  -q, --quiet          只输出关键结果\n"
                "      --metrics        输出一行规范化指标 (CI 跨平台一致性比对)\n"
                "      --notes N        逐音明细条数\n"
                "      --list           列出可用乐器\n";
            return 0;
        }
        else if (!arg.empty() && arg[0] != '-') { input_file = arg; }
        else { std::cerr << "Unknown option: " << arg << "\n"; return 1; }
    }

    if (list) { print_list(); return 0; }

    if (input_file.empty()) {
        std::cerr << "Error: missing input file\n"
                  << "用法: save <file.rcp> [--timbre name] [--output out.wav] [--analyze]\n";
        return 1;
    }

    try {
        const std::string content = read_file(input_file);
        // --check 用严格模式: token 语法错误直接抛出并带行号, 便于 CI 精确定位
        RcpDocument doc = check ? parse_rcp_strict(content) : parse_rcp(content);

        // 音色优先级: 文件内 @timbre/@acoustic > 命令行 --timbre > 默认钢琴
        // (--force-timbre 强制覆盖文件内设定, 适合同谱 A/B 对比音色)
        if (!force_timbre.empty()) {
            doc.has_acoustic = false;
            doc.has_harmonics = false;
            doc.timbre_name.clear();
            resolve_timbre(doc, force_timbre);
            std::cout << "(已用 --force-timbre 覆盖文件内音色为 " << force_timbre << ")\n";
        } else if (doc.has_acoustic || doc.has_harmonics) {
            if (timbre_given && doc.timbre_name.empty())
                doc.warnings.push_back("文件已内置音色, 忽略命令行 --timbre " + timbre_name
                                       + " (需要强制覆盖请用 --force-timbre)");
        } else {
            resolve_timbre(doc, timbre_name);
        }

        RenderStats st;
        auto audio = render_document(doc, opt, (analyze || check) ? &st : nullptr);
        if (audio.empty()) {
            std::cerr << "No audio generated (无音符或全部为休止符).\n";
            return 1;
        }

        // ── 校验模式 (CI): 只体检, 不写盘, 有问题以非 0 退出 ──────
        if (check) {
            if (analyze) print_analysis(doc, st, audio, opt.stereo, opt.sample_rate, note_detail);
            return run_check(doc, st);
        }

        if (output_file.empty()) {
            fs::path p(input_file);
            std::string tag = doc.timbre_name.empty() ? timbre_name : doc.timbre_name;
            output_file = "output_" + p.stem().string() + "_" + tag + ".wav";
        }

        const int channels = opt.stereo ? 2 : 1;
        if (!write_wav(output_file, audio, opt.sample_rate, channels)) {
            std::cerr << "Failed to write WAV: " << output_file << "\n";
            return 1;
        }

        if (!quiet) {
            const double frames = static_cast<double>(audio.size()) / channels;
            std::cout << "转换完成: " << output_file << "\n"
                      << "  乐器: " << (doc.timbre_name.empty() ? timbre_name : doc.timbre_name)
                      << "   音符: " << doc.notes.size()
                      << "   时长: " << (frames / opt.sample_rate) << " 秒"
                      << "   声道: " << channels << "\n";
        }

        if (metrics) print_metrics(input_file, st, static_cast<int>(doc.notes.size()));
        else if (analyze) print_analysis(doc, st, audio, opt.stereo, opt.sample_rate, note_detail);
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
