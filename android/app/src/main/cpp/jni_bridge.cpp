/**
 * jni_bridge.cpp — 将 music_editor 的 C++ 核心移植到 Android 的 JNI 桥接层
 *
 * 对外暴露给 Kotlin 的三个能力 (与桌面版 save 工具行为一致):
 *   1. getTimbres()  → 可用音色名称列表
 *   2. renderPcm()   → 把 RCP 文本渲染成 16-bit PCM 采样 (用于 AudioTrack 播放)
 *   3. renderWav()   → 把 RCP 文本渲染成完整 WAV 文件字节 (用于导出)
 */

#include <jni.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "note_parser.h"
#include "tone_gen.h"
#include "wav_writer.h"

// ── 渲染核心 (与 save/main.cpp 的 render_rcp 一致) ──────────────────
// 解析 RCP 内容 → 生成波形, 音符之间加 50ms 间隔 (与 player/save 对齐)
static std::vector<float> render_rcp(const std::string& content,
                                     const std::vector<double>& harmonics,
                                     int sample_rate,
                                     const std::vector<HarmonicSustain>& sustain = {})
{
    // 去除 UTF-8 BOM (从 Windows 记事本等粘贴的内容可能带有)
    size_t start = 0;
    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        start = 3;
    }

    std::stringstream ss(content.substr(start));
    std::string first_line;
    std::getline(ss, first_line);

    double bpm, base_freq, base_beat_dur;
    if (!parse_rcp_header(first_line, bpm, base_freq, base_beat_dur))
        throw std::runtime_error("RCP 头部无效: " + first_line);

    NoteParser parser(base_freq, base_beat_dur);
    std::vector<float> audio;

    std::string line;
    while (std::getline(ss, line)) {
        std::stringstream ls(line);
        std::string token;
        while (ls >> token) {
            if (token.empty()) continue;
            auto note = parser.parse(token);
            auto tone = generate_tone(note.frequency, note.duration_sec,
                                      harmonics, sample_rate, sustain);
            audio.insert(audio.end(), tone.begin(), tone.end());
            add_gap(audio, 0.05, sample_rate);  // 音符间隔 50ms
        }
    }
    return audio;
}

// float32 [-1,1] → int16 PCM
static std::vector<int16_t> to_pcm16(const std::vector<float>& samples)
{
    std::vector<int16_t> pcm(samples.size());
    for (size_t i = 0; i < samples.size(); ++i) {
        float s = samples[i];
        if (s > 1.0f) s = 1.0f;
        else if (s < -1.0f) s = -1.0f;
        pcm[i] = static_cast<int16_t>(s * 32767.0f);
    }
    return pcm;
}

// ── JNI 异常辅助 ──────────────────────────────────────────────────
static void throw_runtime(JNIEnv* env, const std::string& msg)
{
    jclass cls = env->FindClass("java/lang/RuntimeException");
    if (cls) env->ThrowNew(cls, msg.c_str());
    env->DeleteLocalRef(cls);
}

// ── 工具: Java String → std::string ──────────────────────────────
static std::string jstring_to_std(JNIEnv* env, jstring js)
{
    if (!js) return {};
    const char* chars = env->GetStringUTFChars(js, nullptr);
    if (!chars) return {};
    std::string s(chars);
    env->ReleaseStringUTFChars(js, chars);
    return s;
}

// ── 工具: std::vector 采样 → Java byte[] ─────────────────────────
static jbyteArray bytes_to_jbytearray(JNIEnv* env, const void* data, size_t n)
{
    jbyteArray arr = env->NewByteArray(static_cast<jsize>(n));
    if (!arr) return nullptr;
    env->SetByteArrayRegion(arr, 0, static_cast<jsize>(n),
                            static_cast<const jbyte*>(data));
    return arr;
}

// ── 工具: Java double[] → std::vector<double> ────────────────────
static std::vector<double> jdoublearray_to_std(JNIEnv* env, jdoubleArray arr)
{
    std::vector<double> h;
    if (!arr) return h;
    jsize n = env->GetArrayLength(arr);
    if (n <= 0) return h;
    h.resize(static_cast<size_t>(n));
    env->GetDoubleArrayRegion(arr, 0, n, h.data());
    return h;
}

// ── 公共渲染入口 (前向声明) ──────────────────────────────────────
static jbyteArray render_pcm_or_wav(JNIEnv* env, jstring content, jdoubleArray harmonics,
                                    jstring sustainLine, jint sample_rate, bool want_wav);

// ── 1. getTimbres(): String[] ─────────────────────────────────────
extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_music_editor_MusicNative_getTimbres(JNIEnv* env, jclass /*clazz*/)
{
    const auto& timbres = Timbres::all();
    jclass string_cls = env->FindClass("java/lang/String");
    if (!string_cls) return nullptr;

    jobjectArray arr = env->NewObjectArray(
        static_cast<jsize>(timbres.size()), string_cls, nullptr);
    if (!arr) return nullptr;

    for (size_t i = 0; i < timbres.size(); ++i) {
        jstring s = env->NewStringUTF(timbres[i].name.c_str());
        env->SetObjectArrayElement(arr, static_cast<jsize>(i), s);
        env->DeleteLocalRef(s);
    }
    return arr;
}

// ── 2. getTimbreHarmonics(name): double[] ─────────────────────────
// 返回内置音色各倍频振幅 (第 0 个为基频), 供自定义音色时参考/使用
extern "C" JNIEXPORT jdoubleArray JNICALL
Java_com_music_editor_MusicNative_getTimbreHarmonics(JNIEnv* env, jclass /*clazz*/,
                                                     jstring name)
{
    std::string name_str = jstring_to_std(env, name);
    const Timbre* t = Timbres::find_by_name(name_str);
    if (!t) t = Timbres::find_by_name("piano");

    jdoubleArray arr = env->NewDoubleArray(static_cast<jsize>(t->harmonics.size()));
    if (!arr) return nullptr;
    env->SetDoubleArrayRegion(arr, 0, static_cast<jsize>(t->harmonics.size()),
                              t->harmonics.data());
    return arr;
}

// ── 3. renderPcm(content, harmonics, sampleRate): byte[] ──────────
// harmonics: 各倍频振幅, 第 0 个为基频; 返回 int16 PCM (小端), 用于 AudioTrack 播放
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_music_editor_MusicNative_renderPcm(JNIEnv* env, jclass /*clazz*/,
                                            jstring content, jdoubleArray harmonics,
                                            jint sample_rate)
{
    return render_pcm_or_wav(env, content, harmonics, nullptr, sample_rate, false);
}

// ── 3b. renderPcmSustain(content, harmonics, sustainLine, sampleRate): byte[] ──
// sustainLine: 第三行持续比例 (各倍频的时间窗口), 空格分隔, 可空
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_music_editor_MusicNative_renderPcmSustain(JNIEnv* env, jclass /*clazz*/,
                                                   jstring content, jdoubleArray harmonics,
                                                   jstring sustainLine, jint sample_rate)
{
    return render_pcm_or_wav(env, content, harmonics, sustainLine, sample_rate, false);
}

// ── 4. renderWav(content, harmonics, sampleRate): byte[] ──────────
// 返回完整 WAV 文件字节, 可直接写盘/分享
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_music_editor_MusicNative_renderWav(JNIEnv* env, jclass /*clazz*/,
                                            jstring content, jdoubleArray harmonics,
                                            jint sample_rate)
{
    return render_pcm_or_wav(env, content, harmonics, nullptr, sample_rate, true);
}

// ── 4b. renderWavSustain(content, harmonics, sustainLine, sampleRate): byte[] ──
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_music_editor_MusicNative_renderWavSustain(JNIEnv* env, jclass /*clazz*/,
                                                   jstring content, jdoubleArray harmonics,
                                                   jstring sustainLine, jint sample_rate)
{
    return render_pcm_or_wav(env, content, harmonics, sustainLine, sample_rate, true);
}

// ── 公共渲染入口 (PCM / WAV) ──────────────────────────────────────
static jbyteArray render_pcm_or_wav(JNIEnv* env, jstring content, jdoubleArray harmonics,
                                    jstring sustainLine, jint sample_rate, bool want_wav)
{
    try {
        std::string content_str = jstring_to_std(env, content);
        auto h = jdoublearray_to_std(env, harmonics);
        if (h.empty()) {
            const Timbre* t = Timbres::find_by_name("piano");
            h = t->harmonics;
        }

        // 解析第三行持续比例 (可为空 → 全程恒有)
        std::vector<HarmonicSustain> sustain;
        std::string sustain_str = jstring_to_std(env, sustainLine);
        if (!sustain_str.empty())
            sustain = parse_sustain_line(sustain_str);

        auto audio = render_rcp(content_str, h, sample_rate, sustain);
        auto pcm   = to_pcm16(audio);

        if (want_wav) {
            auto wav = encode_wav(pcm, sample_rate);
            return bytes_to_jbytearray(env, wav.data(), wav.size());
        }
        return bytes_to_jbytearray(env, pcm.data(), pcm.size() * sizeof(int16_t));
    } catch (const std::exception& e) {
        throw_runtime(env, e.what());
        return nullptr;
    }
}
