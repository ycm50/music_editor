#ifndef MUSIC_WAV_WRITER_H
#define MUSIC_WAV_WRITER_H

#include <vector>
#include <string>
#include <string_view>
#include <cstdint>

/**
 * 将 int16 PCM 采样编码为完整 WAV 文件字节 (16-bit PCM, 44 字节 RIFF 头 + 数据)
 *
 * 内存中编码, 供导出/分享等需要直接拿到字节的场景使用 (如 Android JNI)。
 *
 * @param pcm           int16 采样数据 (小端)
 * @param sample_rate   采样率 (Hz)
 * @param channels      声道数 (默认 1)
 */
std::vector<uint8_t> encode_wav(const std::vector<int16_t>& pcm,
                                int sample_rate = 44100,
                                int channels = 1);

/**
 * 将 24-bit PCM 字节流编码为完整 WAV 文件字节 (48 字节 RIFF 头)
 *
 * @param pcm24         交错小端 24-bit 采样字节 (每采样 3 字节)
 */
std::vector<uint8_t> encode_wav24(const std::vector<uint8_t>& pcm24,
                                  int sample_rate = 44100,
                                  int channels = 1);

/**
 * float32 → int16 (四舍五入 + 可选 TPDF dither)
 *
 * 物理/听感上: 直接截断会引入与信号相关的量化谐波; TPDF dither (±1 LSB, 三角分布)
 * 把量化误差变成与信号无关的白噪, 电平低到听不见, 但能消除量化失真。
 * 采样数固定时输出完全确定 (确定性 PRNG)。
 */
std::vector<int16_t> to_pcm16(const std::vector<float>& samples,
                              bool dither = true, uint32_t seed = 0);

/// float32 → 24-bit 交错字节 (每采样 3 字节, 小端)
std::vector<uint8_t> to_pcm24(const std::vector<float>& samples,
                              bool dither = true, uint32_t seed = 0);

/**
 * 将 float32 采样数据写入 WAV 文件
 *
 * 立体声时 samples 为左右交错 (L,R,L,R...), 帧数 = samples.size()/channels
 *
 * @param bits          16 或 24 (位深)
 * @param dither        true = 加 TPDF dither (仅 16-bit 有明显意义)
 * @return true 成功, false 失败 (打开/写入错误)
 */
bool write_wav(std::string_view path,
               const std::vector<float>& samples,
               int sample_rate = 44100,
               int channels = 1,
               int bits = 16,
               bool dither = true);

#endif // MUSIC_WAV_WRITER_H
