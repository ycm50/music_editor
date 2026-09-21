/**
 * player — RCP 音乐播放器
 *
 * 用法:
 *   player <file.rcp> [--instrument piano|violin|flute|guitar|harp|bells|music_box|organ]
 *                     [--mono] [--no-reverb] [--timbre 旧式裸谐波音色]
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

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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

// ── 渲染: 解析 + 按物理模型合成 ───────────────────────────────────
static std::vector<float> render_rcp_float(const std::string& content,
                                           const std::string& timbre_name,
                                           bool timbre_given,
                                           const RenderOptions& opt)
{
    RcpDocument doc = parse_rcp(content);

    // 音色优先级: 文件内 @timbre/@acoustic > 命令行 --instrument/--timbre > 钢琴
    if (!doc.has_acoustic && !doc.has_harmonics) {
        std::string lower;
        for (char c : timbre_name)
            lower.push_back(static_cast<char>(std::tolower((unsigned char)c)));
        if (const AcousticParams* p = Instruments::find_by_name(lower)) {
            doc.acoustic = *p;
            doc.has_acoustic = true;
            doc.timbre_name = p->name;
        } else if (const Timbre* t = Timbres::find_by_name(lower)) {
            doc.harmonics = t->harmonics;
            doc.has_harmonics = true;
            doc.timbre_name = t->name;
        } else {
            throw std::runtime_error("Unknown instrument: " + timbre_name);
        }
    } else if (timbre_given && doc.timbre_name.empty()) {
        std::cerr << "提示: 文件已内置音色, 忽略命令行指定的 " << timbre_name << "\n";
    }

    for (const auto& w : doc.warnings)
        std::cerr << "提示: " << w << "\n";

    return render_document(doc, opt, nullptr);
}

// ── 主函数 ─────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("player");
    QCoreApplication::setApplicationVersion("2.0.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("RCP music player");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("file", "RCP file path (use '-' for stdin)");

    QCommandLineOption instrumentOpt(
        QStringList{"i", "instrument", "timbre"},
        "Instrument: piano, violin, flute, guitar, harp, bells, music_box, organ",
        "name", "piano");
    QCommandLineOption monoOpt(QStringList{"mono"}, "Mono output (default stereo + reverb)");
    QCommandLineOption noRevOpt(QStringList{"no-reverb"}, "Disable reverb");
    parser.addOption(instrumentOpt);
    parser.addOption(monoOpt);
    parser.addOption(noRevOpt);

    parser.process(app);

    const QStringList args = parser.positionalArguments();
    if (args.isEmpty()) {
        parser.showHelp(1);
        return 1;
    }

    std::string timbre_name = parser.value(instrumentOpt).toStdString();
    bool timbre_given = !timbre_name.empty() && timbre_name != "piano";
    bool stereo = !parser.isSet(monoOpt);

    RenderOptions ropt;
    ropt.sample_rate = 44100;
    ropt.stereo = stereo;
    if (parser.isSet(noRevOpt)) ropt.reverb_wet = 0.0;

    try {
        std::string content = read_file(args[0].toStdString());
        auto samples_f = render_rcp_float(content, timbre_name, timbre_given, ropt);

        if (samples_f.empty()) {
            std::cerr << "No audio generated.\n";
            return 1;
        }

        const int channels = stereo ? 2 : 1;
        const size_t frames = samples_f.size() / static_cast<size_t>(channels);
        std::cout << "音频时长: " << (static_cast<double>(frames) / ropt.sample_rate)
                  << " s  声道: " << channels << "\n";

        // ── 音频格式: 优先 Float32 (WASAPI/FFmpeg 后端通用), 回退 Int16 ──
        QAudioFormat fmt;
        fmt.setSampleRate(ropt.sample_rate);
        fmt.setChannelCount(channels);
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
                fmt = outDev.preferredFormat();
                std::cerr << "Warning: 目标格式不被支持, 使用设备首选格式 "
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
        std::cout << "Audio format: " << (int16Out ? "Int16" : "Float32")
                  << " " << fmt.sampleRate() << " Hz\n";
#else
        const bool int16Out = true;
#endif
        const int bytesPerSample = int16Out ? 2 : 4;
        const int devChannels = fmt.channelCount() > 0 ? fmt.channelCount() : channels;

        // ── 分块送数据到音频设备 ────────────────────────────────────
        std::vector<float> samples(std::move(samples_f));
        std::vector<int16_t> pcmBuf;
        std::vector<float> convBuf;
        size_t writePos = 0;   // 单位: 设备帧

        // 若设备声道数与渲染不一致, 做一次简单上/下混
        auto adapt = [&](const float* src, size_t inFrames) -> const float* {
            if (devChannels == channels) return src;
            convBuf.resize(inFrames * static_cast<size_t>(devChannels));
            for (size_t i = 0; i < inFrames; ++i) {
                if (channels == 1 && devChannels == 2) {
                    const float v = src[i];
                    convBuf[i * 2] = v;
                    convBuf[i * 2 + 1] = v;
                } else {
                    for (int c = 0; c < devChannels; ++c)
                        convBuf[i * static_cast<size_t>(devChannels) + static_cast<size_t>(c)] =
                            src[i * static_cast<size_t>(channels) + static_cast<size_t>(c % channels)];
                }
            }
            return convBuf.data();
        };

        auto writeChunk = [&](const float* src, size_t inFrames) -> qint64 {
            const float* p = adapt(src, inFrames);
            if (int16Out) {
                pcmBuf.resize(inFrames * static_cast<size_t>(devChannels));
                for (size_t i = 0; i < pcmBuf.size(); ++i) {
                    float s = p[i];
                    if (s > 1.0f) s = 1.0f;
                    else if (s < -1.0f) s = -1.0f;
                    pcmBuf[i] = static_cast<int16_t>(s * 32767.0f);
                }
                return dev->write(reinterpret_cast<const char*>(pcmBuf.data()),
                                  static_cast<qint64>(pcmBuf.size()) * 2);
            }
            return dev->write(reinterpret_cast<const char*>(p),
                              static_cast<qint64>(inFrames * static_cast<size_t>(devChannels)) * 4);
        };

        QTimer* feedTimer = new QTimer(&app);
        QObject::connect(feedTimer, &QTimer::timeout, [&]() {
            if (Q_UNLIKELY(!dev || !sink)) return;
            if (writePos >= frames) {
                feedTimer->stop();
                return;
            }
            int freeBytes = sink->bytesFree();
            if (freeBytes <= 0) return;

            const int bytesPerFrame = bytesPerSample * devChannels;
            size_t canWrite = static_cast<size_t>(freeBytes) / static_cast<size_t>(bytesPerFrame);
            size_t toWrite = std::min(canWrite, frames - writePos);
            if (toWrite == 0) return;

            qint64 written = writeChunk(samples.data() + writePos * static_cast<size_t>(channels),
                                        toWrite);
            if (written > 0)
                writePos += static_cast<size_t>(written) / static_cast<size_t>(bytesPerFrame);
        });

        QObject::connect(sink, &QAudioSink::stateChanged, [&](QAudio::State st) {
            if (st == QAudio::IdleState && writePos >= frames) {
                QTimer::singleShot(200, &app, [&]() {
                    sink->stop();
                    app.quit();
                });
            } else if (st == QAudio::StoppedState) {
                app.quit();
            }
        });

        int free0 = sink->bytesFree();
        if (free0 > 0) {
            const int bytesPerFrame = bytesPerSample * devChannels;
            size_t n0 = std::min(frames,
                                 static_cast<size_t>(free0) / static_cast<size_t>(bytesPerFrame));
            qint64 w0 = writeChunk(samples.data(), n0);
            if (w0 > 0)
                writePos += static_cast<size_t>(w0) / static_cast<size_t>(bytesPerFrame);
        }

        feedTimer->start(20);
        return app.exec();

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
