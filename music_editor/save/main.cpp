/**
 * save — RCP → WAV 转换器
 *
 * 用法:
 *   save <file.rcp> [--timbre piano|violin|flute] [--output out.wav]
 *
 * 依赖: music-core (无需 Qt)
 */

#include "note_parser.h"
#include "tone_gen.h"
#include "wav_writer.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

// ── 读取文件 ───────────────────────────────────────────────────────
static std::string read_file(const std::string& path)
{
    std::ifstream ifs(path);
    if (!ifs.is_open())
        throw std::runtime_error("Cannot open file: " + path);
    std::istreambuf_iterator<char> begin(ifs), end;
    return {begin, end};
}

// ── 渲染音乐 ───────────────────────────────────────────────────────
static std::vector<float> render_rcp(const std::string& content,
                                     const std::vector<double>& fallback_harmonics,
                                     size_t* note_count = nullptr)
{
    // 统一 RCP 格式: 文件内可内嵌音色行(第2行)与持续比例行(第3行)
    return render_rcp_unified(content, fallback_harmonics, 44100, nullptr, note_count);
}

// ── 主函数 ─────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    // 简单手动参数解析 (无需外部依赖)
    std::string input_file;
    std::string output_file;
    std::string timbre_name = "piano";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--timbre" || arg == "-t") {
            if (i + 1 < argc) timbre_name = argv[++i];
            else { std::cerr << "Error: --timbre requires a value\n"; return 1; }
        } else if (arg == "--output" || arg == "-o") {
            if (i + 1 < argc) output_file = argv[++i];
            else { std::cerr << "Error: --output requires a value\n"; return 1; }
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "用法: save <file.rcp> [--timbre piano|violin|flute] [--output out.wav]\n";
            return 0;
        } else if (arg[0] != '-') {
            input_file = arg;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return 1;
        }
    }

    if (input_file.empty()) {
        std::cerr << "Error: missing input file\n"
                  << "用法: save <file.rcp> [--timbre name] [--output out.wav]\n";
        return 1;
    }

    // 查找音色
    const Timbre* timbre = Timbres::find_by_name(timbre_name);
    if (!timbre) {
        std::cerr << "Unknown timbre: " << timbre_name << "\n"
                  << "Available: ";
        for (const auto& t : Timbres::all())
            std::cerr << t.name << " ";
        std::cerr << "\n";
        return 1;
    }

    try {
        auto content = read_file(input_file);
        size_t note_count = 0;
        auto audio = render_rcp(content, timbre->harmonics, &note_count);

        if (audio.empty()) {
            std::cerr << "No audio generated.\n";
            return 1;
        }

        // 自动生成输出文件名
        if (output_file.empty()) {
            fs::path p(input_file);
            output_file = "output_" + p.stem().string() + "_" + timbre->name + ".wav";
        }

        bool ok = write_wav(output_file, audio);
        if (!ok) {
            std::cerr << "Failed to write WAV file: " << output_file << "\n";
            return 1;
        }

        std::cout << "转换完成: " << output_file << "\n"
                  << "  音符数: " << note_count << "\n"
                  << "  时长: " << (audio.size() / 44100.0) << " 秒\n"
                  << "  音色: " << timbre->name << "\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
