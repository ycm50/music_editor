/**
 * jni_bridge.cpp — 将 music_editor 的 C++ 核心移植到 Android 的 JNI 桥接层
 *
 * 对外暴露给 Kotlin 的能力 (与桌面版 save 工具行为一致):
 *   1. getTimbres()          → 可用音色/乐器名称列表
 *   2. getTimbreHarmonics()  → 内置音色的谐波振幅 (兼容旧接口)
 *   3. getInstruments()      → 物理乐器名称列表
 *   4. renderPcm()           → 渲染成 16-bit PCM (AudioTrack 播放)
 *   5. renderWav()           → 渲染成完整 WAV 文件字节 (导出)
 *   6. analyze()             → 乐理/声学体检报告文本
 *
 * 说明: 解析与合成**全部**走 core 的 parse_rcp / render_document, 不再自行解析,
 *       从而与桌面端严格一致 (旧版本这里是第二套解析逻辑, 容易漂移)。
 */

#include <jni.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ai_prompt.h"
#include "note_parser.h"
#include "tone_gen.h"
#include "wav_writer.h"

// ── float32 [-1,1] → int16 PCM ────────────────────────────────────
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

static std::string jstring_to_std(JNIEnv* env, jstring js)
{
    if (!js) return {};
    const char* chars = env->GetStringUTFChars(js, nullptr);
    if (!chars) return {};
    std::string s(chars);
    env->ReleaseStringUTFChars(js, chars);
    return s;
}

static jbyteArray bytes_to_jbytearray(JNIEnv* env, const void* data, size_t n)
{
    jbyteArray arr = env->NewByteArray(static_cast<jsize>(n));
    if (!arr) return nullptr;
    env->SetByteArrayRegion(arr, 0, static_cast<jsize>(n),
                            static_cast<const jbyte*>(data));
    return arr;
}

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

/**
 * 解析 + 渲染 (统一入口)
 *
 * 音色优先级与桌面端一致: 文件内 @timbre/@acoustic > 文件内旧式谐波行
 *                        > fallback_harmonics (Kotlin 侧传入的第 2 行)
 */
static std::vector<float> render_rcp(const std::string& content,
                                     const std::vector<double>& fallback_harmonics,
                                     int sample_rate,
                                     bool stereo,
                                     RenderStats* stats)
{
    RcpDocument doc = parse_rcp(content);
    if (!doc.has_harmonics && !doc.has_acoustic && !fallback_harmonics.empty()) {
        doc.harmonics = fallback_harmonics;
        doc.has_harmonics = true;
    }

    RenderOptions opt;
    opt.sample_rate = sample_rate;
    opt.stereo = stereo;
    opt.reverb_wet = stereo ? 0.28 : 0.0;
    return render_document(doc, opt, stats);
}

// ── 前向声明: renderPcm/renderWav 为其 Sustain 变体的薄封装 ──────
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_music_editor_MusicNative_renderPcmSustain(JNIEnv*, jclass, jstring, jdoubleArray,
                                                   jstring, jint, jboolean);
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_music_editor_MusicNative_renderWavSustain(JNIEnv*, jclass, jstring, jdoubleArray,
                                                   jstring, jint, jboolean);

// ── 1. getTimbres(): String[] ─────────────────────────────────────
extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_music_editor_MusicNative_getTimbres(JNIEnv* env, jclass /*clazz*/){
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

// ── 2. getInstruments(): String[] (物理乐器) ─────────────────────
extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_music_editor_MusicNative_getInstruments(JNIEnv* env, jclass /*clazz*/)
{
    const auto& list = Instruments::all();
    jclass string_cls = env->FindClass("java/lang/String");
    if (!string_cls) return nullptr;

    jobjectArray arr = env->NewObjectArray(static_cast<jsize>(list.size()), string_cls, nullptr);
    if (!arr) return nullptr;

    for (size_t i = 0; i < list.size(); ++i) {
        jstring s = env->NewStringUTF(list[i].name.c_str());
        env->SetObjectArrayElement(arr, static_cast<jsize>(i), s);
        env->DeleteLocalRef(s);
    }
    return arr;
}

// ── 3. getTimbreHarmonics(name): double[] ────────────────────────
extern "C" JNIEXPORT jdoubleArray JNICALL
Java_com_music_editor_MusicNative_getTimbreHarmonics(JNIEnv* env, jclass /*clazz*/,
                                                     jstring name)
{
    std::string name_str = jstring_to_std(env, name);

    std::vector<double> h;
    if (const Timbre* t = Timbres::find_by_name(name_str)) {
        h = t->harmonics;
    } else if (const AcousticParams* p = Instruments::find_by_name(name_str)) {
        h = harmonics_from_acoustic(*p);
    } else {
        h = Timbres::PIANO.harmonics;
    }

    jdoubleArray arr = env->NewDoubleArray(static_cast<jsize>(h.size()));
    if (!arr) return nullptr;
    env->SetDoubleArrayRegion(arr, 0, static_cast<jsize>(h.size()), h.data());
    return arr;
}

// ── 4. renderPcm(content, harmonics, sampleRate, stereo): byte[] ─
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_music_editor_MusicNative_renderPcm(JNIEnv* env, jclass /*clazz*/,
                                            jstring content, jdoubleArray harmonics,
                                            jint sample_rate, jboolean stereo)
{
    return Java_com_music_editor_MusicNative_renderPcmSustain(
        env, nullptr, content, harmonics, nullptr, sample_rate, stereo);
}

// ── 4b. renderPcmSustain(...): byte[] ────────────────────────────
// sustainLine 参数保留以兼容旧调用点, 新格式请直接写在内容里 (@acoustic / 持续比例行)
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_music_editor_MusicNative_renderPcmSustain(JNIEnv* env, jclass /*clazz*/,
                                                   jstring content, jdoubleArray harmonics,
                                                   jstring /*sustainLine*/, jint sample_rate,
                                                   jboolean stereo)
{
    try {
        std::string content_str = jstring_to_std(env, content);
        auto h = jdoublearray_to_std(env, harmonics);

        auto audio = render_rcp(content_str, h, sample_rate, stereo == JNI_TRUE, nullptr);
        auto pcm = to_pcm16(audio);
        return bytes_to_jbytearray(env, pcm.data(), pcm.size() * sizeof(int16_t));
    } catch (const std::exception& e) {
        throw_runtime(env, e.what());
        return nullptr;
    }
}

// ── 5. renderWav(content, harmonics, sampleRate, stereo): byte[] ─
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_music_editor_MusicNative_renderWav(JNIEnv* env, jclass /*clazz*/,
                                            jstring content, jdoubleArray harmonics,
                                            jint sample_rate, jboolean stereo)
{
    return Java_com_music_editor_MusicNative_renderWavSustain(
        env, nullptr, content, harmonics, nullptr, sample_rate, stereo);
}

// ── 5b. renderWavSustain(...): byte[] ────────────────────────────
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_music_editor_MusicNative_renderWavSustain(JNIEnv* env, jclass /*clazz*/,
                                                   jstring content, jdoubleArray harmonics,
                                                   jstring /*sustainLine*/, jint sample_rate,
                                                   jboolean stereo)
{
    try {
        std::string content_str = jstring_to_std(env, content);
        auto h = jdoublearray_to_std(env, harmonics);
        const bool st = (stereo == JNI_TRUE);

        auto audio = render_rcp(content_str, h, sample_rate, st, nullptr);
        auto pcm = to_pcm16(audio);
        auto wav = encode_wav(pcm, sample_rate, st ? 2 : 1);
        return bytes_to_jbytearray(env, wav.data(), wav.size());
    } catch (const std::exception& e) {
        throw_runtime(env, e.what());
        return nullptr;
    }
}

// ── 7. getScorePrompt(): String ──────────────────────────────────
// 返回生成乐谱的系统提示词。正文唯一来源是 core/ai_prompt.cpp,
// 与桌面端共用同一份文本, 避免两端各写一份而漂移。
extern "C" JNIEXPORT jstring JNICALL
Java_com_music_editor_MusicNative_getScorePrompt(JNIEnv* env, jclass /*clazz*/)
{
    const std::string_view p = score_system_prompt_view();
    return env->NewStringUTF(std::string(p).c_str());
}

// ── 8. getScorePromptExample(): String ──────────────────────────
// 提示词里那段示例乐谱 (供"示例"按钮/自检使用)
extern "C" JNIEXPORT jstring JNICALL
Java_com_music_editor_MusicNative_getScorePromptExample(JNIEnv* env, jclass /*clazz*/)
{
    const std::string_view e = score_example_rcp();
    return env->NewStringUTF(std::string(e).c_str());
}

// ── 6. analyze(content, sampleRate): String ──────────────────────
// 返回人类可读的乐理/声学体检报告 (供"检查乐谱"按钮)
extern "C" JNIEXPORT jstring JNICALL
Java_com_music_editor_MusicNative_analyze(JNIEnv* env, jclass /*clazz*/,
                                          jstring content, jint sample_rate)
{
    try {
        std::string content_str = jstring_to_std(env, content);
        RcpDocument doc = parse_rcp(content_str);

        RenderOptions opt;
        opt.sample_rate = sample_rate;
        opt.stereo = false;
        opt.reverb_wet = 0.0;
        RenderStats st;
        auto audio = render_document(doc, opt, &st);

        std::ostringstream os;
        os << std::fixed << std::setprecision(3);
        os << "乐器: " << (st.instrument.empty() ? "(裸谐波表)" : st.instrument) << "\n";
        os << "时长: " << st.duration_sec << " s\n";
        os << "音符/休止: " << st.note_count << " / " << st.rest_count
           << "  和弦块 " << st.chords << "  最大同时发声 " << st.max_polyphony << "\n";
        os << "峰值/RMS: " << st.peak << " / " << st.rms
           << "  峰均比 " << st.crest_db << " dB\n";
        os << "直流: " << std::scientific << st.dc_offset << std::fixed
           << "  混叠丢弃分音: " << st.discarded_total << "\n";
        if (st.bar_sec > 0.0)
            os << "拍号 " << doc.meter_num << "/" << doc.meter_den
               << "  小节数 " << st.bars << "  对齐偏差 " << st.bar_fit_error << " s\n";
        for (const auto& w : st.warnings) os << "! " << w << "\n";
        if (audio.empty()) os << "! 未生成音频\n";

        return env->NewStringUTF(os.str().c_str());
    } catch (const std::exception& e) {
        throw_runtime(env, e.what());
        return nullptr;
    }
}
