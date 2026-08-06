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
 * 与 write_wav 共用同一套 WAV 头逻辑。
 *
 * @param pcm           int16 采样数据 (小端)
 * @param sample_rate   采样率 (Hz)
 * @param channels      声道数 (默认 1)
 * @return 完整 WAV 文件字节
 */
std::vector<uint8_t> encode_wav(const std::vector<int16_t>& pcm,
                                int sample_rate = 44100,
                                int channels = 1);

/**
 * 将 float32 采样数据写入 WAV 文件 (16-bit PCM, mono)
 *
 * @param path          输出路径
 * @param samples       float32 采样, 值域 [-1.0, 1.0]
 * @param sample_rate   采样率 (Hz)
 * @param channels      声道数 (默认 1)
 * @return true 成功, false 失败 (打开/写入错误)
 */
bool write_wav(std::string_view path,
               const std::vector<float>& samples,
               int sample_rate = 44100,
               int channels = 1);

#endif // MUSIC_WAV_WRITER_H
