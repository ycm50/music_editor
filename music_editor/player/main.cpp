/**
 * player — RCP 音乐播放器
 *
 * 用法:
 *   player <file.rcp> [--timbre piano|violin|flute]
 *
 * 依赖: Qt Multimedia, music-core
 */

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QTimer>
#include <QAudioFormat>
#include <QIODevice>
#include <QByteArray>
#include <QDebug>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QAudioSink>
#include <QMediaDevices>
#else
#include <QAudioOutput>
#include <QAudioDeviceInfo>
#endif

#include "note_parser.h"
#include "tone_gen.h"

#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <cstdint>

// ── 读取整个文件到字符串 ──────────────────────────────────────────
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

// ── 解析 RCP 内容，生成 float32 音频 ([-1,1]) ─────────────────────
static std::vector<float> render_rcp_float(const std::string& content,
                                           const std::vector<double>& fallback_harmonics)
{
    // 统一 RCP 格式: 文件内可内嵌音色行(第2行)与持续比例行(第3行)
    return render_rcp_unified(content, fallback_harmonics, 44100);
}

// ── 主函数 ─────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("player");
    QCoreApplication::setApplicationVersion("1.0.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("RCP music player");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("file", "RCP file path (use '-' for stdin)");

    QCommandLineOption timbreOpt(
        QStringList{"t", "timbre"},
        "Timbre: piano, violin, flute",
        "name", "piano");
    parser.addOption(timbreOpt);

    parser.process(app);

    const QStringList args = parser.positionalArguments();
    if (args.isEmpty()) {
        parser.showHelp(1);
        return 1;
    }

    std::string timbre_name = parser.value(timbreOpt).toStdString();
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
        std::string content = read_file(args[0].toStdString());
        auto samples_f = render_rcp_float(content, timbre->harmonics);

        if (samples_f.empty()) {
            std::cerr << "No audio generated.\n";
            return 1;
        }

        size_t total_samples = samples_f.size();
        double total_sec = static_cast<double>(total_samples) / 44100.0;
        std::cout << "Audio duration: " << total_sec << " s\n";

        // ── 音频格式: 优先 Float32 (WASAPI/FFmpeg 后端通用), 回退 Int16 ──
        // 避免后端不支持 Int16 时报 "Audio format not supported"
        QAudioFormat fmt;
        fmt.setSampleRate(44100);
        fmt.setChannelCount(1);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        auto outDev = QMediaDevices::defaultAudioOutput();
        if (outDev.isNull()) {
            std::cerr << "未检测到可用的音频输出设备\n";
            return 1;
        }
        fmt.setSampleFormat(QAudioFormat::Float);
        if (!outDev.isFormatSupported(fmt)) {
            fmt.setSampleFormat(QAudioFormat::Int16);
            if (!outDev.isFormatSupported(fmt)) {
                // 最后兜底: 用设备首选格式 (WASAPI 等后端会自动做格式转换)
                fmt = outDev.preferredFormat();
                std::cerr << "Warning: 设备不支持常用格式, 使用设备首选格式 "
                          << fmt.sampleRate() << "Hz " << fmt.channelCount() << "ch\n";
            }
        }
        auto* sink = new QAudioSink(outDev, fmt, &app);
#else
        fmt.setSampleSize(16);
        fmt.setSampleType(QAudioFormat::SignedInt);
        fmt.setByteOrder(QAudioFormat::LittleEndian);
        QAudioDeviceInfo info(QAudioDeviceInfo::defaultOutputDevice());
        if (!info.isFormatSupported(fmt)) {
            std::cerr << "Audio format not supported\n";
            return 1;
        }
        auto* sink = new QAudioOutput(fmt, &app);
#endif

        QIODevice* dev = sink->start();
        if (!dev) {
            std::cerr << "Failed to start audio output\n";
            return 1;
        }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const bool int16Out = (fmt.sampleFormat() == QAudioFormat::Int16);
        std::cout << "Audio format: "
                  << (int16Out ? "Int16" : "Float32")
                  << " " << fmt.sampleRate() << " Hz\n";
#else
        const bool int16Out = true;
#endif
        const int bytesPerSample = int16Out ? 2 : 4;

        // ── 分块送数据到音频设备 (Int16 时按需转换) ────────────────
        std::vector<float> samples(std::move(samples_f));
        std::vector<int16_t> pcmBuf;
        size_t writePos = 0;

        auto writeChunk = [&](const float* src, size_t count) -> qint64 {
            if (int16Out) {
                pcmBuf.resize(count);
                for (size_t i = 0; i < count; ++i) {
                    float s = src[i];
                    if (s > 1.0f) s = 1.0f;
                    else if (s < -1.0f) s = -1.0f;
                    pcmBuf[i] = static_cast<int16_t>(s * 32767.0f);
                }
                return dev->write(reinterpret_cast<const char*>(pcmBuf.data()),
                                  static_cast<qint64>(count) * 2);
            }
            return dev->write(reinterpret_cast<const char*>(src),
                              static_cast<qint64>(count) * 4);
        };

        QTimer* feedTimer = new QTimer(&app);
        QObject::connect(feedTimer, &QTimer::timeout, [&]() {
            if (Q_UNLIKELY(!dev || !sink)) return;

            // 检查是否完成
            if (writePos >= total_samples) {
                // 所有数据已写入, 等 IdleState 触发结束
                feedTimer->stop();
                return;
            }

            int freeBytes = sink->bytesFree();
            if (freeBytes <= 0) return;

            size_t canWrite = static_cast<size_t>(freeBytes) / bytesPerSample;
            size_t remaining = total_samples - writePos;
            size_t toWrite = std::min(canWrite, remaining);

            if (toWrite == 0) return;

            qint64 written = writeChunk(samples.data() + writePos, toWrite);
            if (written > 0)
                writePos += static_cast<size_t>(written) / bytesPerSample;
        });

        // stateChanged: 检测播放结束
        QObject::connect(sink, &QAudioSink::stateChanged, [&](QAudio::State st) {
            if (st == QAudio::IdleState && writePos >= total_samples) {
                QTimer::singleShot(200, &app, [&]() {
                    sink->stop();
                    app.quit();
                });
            } else if (st == QAudio::StoppedState) {
                app.quit();
            }
        });

        // 先写一块数据让音频流启动
        int free0 = sink->bytesFree();
        if (free0 > 0) {
            size_t n0 = std::min(total_samples,
                static_cast<size_t>(free0) / bytesPerSample);
            qint64 w0 = writeChunk(samples.data(), n0);
            if (w0 > 0)
                writePos += static_cast<size_t>(w0) / bytesPerSample;
        }

        feedTimer->start(20);

        return app.exec();

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
