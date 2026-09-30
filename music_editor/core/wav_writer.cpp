#include "wav_writer.h"

#include <fstream>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace {

inline uint32_t hash_u32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/// TPDF dither: 两个独立均匀分布之和 (三角分布), 峰峰 1 LSB
inline double tpdf(uint32_t i, uint32_t seed)
{
    const double a = hash_u32(i * 2654435761u + seed) * (1.0 / 4294967296.0);
    const double b = hash_u32(i * 2246822519u + seed + 0x9e3779b9u) * (1.0 / 4294967296.0);
    return a - b;
}

} // namespace

// WAV 文件头 (RIFF, 16-bit / 24-bit PCM)
struct WavHeader {
    char     riff_id[4]     = {'R', 'I', 'F', 'F'};
    uint32_t riff_size      = 0;     // 文件总长 - 8
    char     wave_id[4]     = {'W', 'A', 'V', 'E'};

    char     fmt_id[4]      = {'f', 'm', 't', ' '};
    uint32_t fmt_size       = 16;    // PCM 头长
    uint16_t audio_format   = 1;     // 1 = PCM
    uint16_t num_channels   = 1;
    uint32_t sample_rate    = 44100;
    uint32_t byte_rate      = 0;     // sample_rate * channels * bytes_per_sample
    uint16_t block_align    = 0;     // channels * bytes_per_sample
    uint16_t bits_per_sample= 16;

    char     data_id[4]     = {'d', 'a', 't', 'a'};
    uint32_t data_size      = 0;     // 采样数据字节数
};

static_assert(sizeof(WavHeader) == 44, "WavHeader must be exactly 44 bytes");

std::vector<uint8_t> encode_wav(const std::vector<int16_t>& pcm,
                                int sample_rate,
                                int channels)
{
    if (sample_rate <= 0) sample_rate = 44100;
    if (channels <= 0) channels = 1;

    constexpr uint32_t bytes_per_sample = 2;  // 16-bit
    uint32_t data_bytes = static_cast<uint32_t>(pcm.size()) * bytes_per_sample;

    std::vector<uint8_t> wav;
    wav.reserve(sizeof(WavHeader) + data_bytes);
    auto put = [&wav](const void* src, size_t n) {
        const auto* p = static_cast<const uint8_t*>(src);
        wav.insert(wav.end(), p, p + n);
    };

    WavHeader hdr;
    hdr.num_channels  = static_cast<uint16_t>(channels);
    hdr.sample_rate   = static_cast<uint32_t>(sample_rate);
    hdr.bits_per_sample = 16;
    hdr.block_align   = static_cast<uint16_t>(channels * (hdr.bits_per_sample / 8));
    hdr.byte_rate     = hdr.sample_rate * hdr.block_align;
    hdr.data_size     = data_bytes;
    hdr.riff_size     = 36 + hdr.data_size;
    put(&hdr, sizeof(hdr));

    put(pcm.data(), pcm.size() * bytes_per_sample);
    return wav;
}

std::vector<uint8_t> encode_wav24(const std::vector<uint8_t>& pcm24,
                                  int sample_rate,
                                  int channels)
{
    if (sample_rate <= 0) sample_rate = 44100;
    if (channels <= 0) channels = 1;

    uint32_t data_bytes = static_cast<uint32_t>(pcm24.size());
    std::vector<uint8_t> wav;
    wav.reserve(sizeof(WavHeader) + data_bytes);
    auto put = [&wav](const void* src, size_t n) {
        const auto* p = static_cast<const uint8_t*>(src);
        wav.insert(wav.end(), p, p + n);
    };

    WavHeader hdr;
    hdr.num_channels   = static_cast<uint16_t>(channels);
    hdr.sample_rate    = static_cast<uint32_t>(sample_rate);
    hdr.bits_per_sample = 24;
    hdr.block_align    = static_cast<uint16_t>(channels * 3);
    hdr.byte_rate      = hdr.sample_rate * hdr.block_align;
    hdr.data_size      = data_bytes;
    hdr.riff_size      = 36 + hdr.data_size;
    put(&hdr, sizeof(hdr));
    if (!pcm24.empty()) put(pcm24.data(), pcm24.size());
    return wav;
}

std::vector<int16_t> to_pcm16(const std::vector<float>& samples, bool dither, uint32_t seed)
{
    std::vector<int16_t> pcm(samples.size());
    for (size_t i = 0; i < samples.size(); ++i) {
        float s = samples[i];
        if (s > 1.0f) s = 1.0f;
        else if (s < -1.0f) s = -1.0f;
        double v = static_cast<double>(s) * 32767.0;
        if (dither) v += tpdf(static_cast<uint32_t>(i), seed);
        long r = std::lround(v);
        r = std::clamp<long>(r, -32768, 32767);
        pcm[i] = static_cast<int16_t>(r);
    }
    return pcm;
}

std::vector<uint8_t> to_pcm24(const std::vector<float>& samples, bool dither, uint32_t seed)
{
    std::vector<uint8_t> out(samples.size() * 3);
    for (size_t i = 0; i < samples.size(); ++i) {
        float s = samples[i];
        if (s > 1.0f) s = 1.0f;
        else if (s < -1.0f) s = -1.0f;
        double v = static_cast<double>(s) * 8388607.0;
        // 24-bit 下 dither 用 ±1 LSB 的 TPDF (抖动幅度按 24-bit LSB 计)
        if (dither) v += tpdf(static_cast<uint32_t>(i), seed);
        long r = std::lround(v);
        r = std::clamp<long>(r, -8388608, 8388607);
        const uint32_t u = static_cast<uint32_t>(r) & 0x00FFFFFFu;
        out[i * 3 + 0] = static_cast<uint8_t>(u & 0xFF);
        out[i * 3 + 1] = static_cast<uint8_t>((u >> 8) & 0xFF);
        out[i * 3 + 2] = static_cast<uint8_t>((u >> 16) & 0xFF);
    }
    return out;
}

bool write_wav(std::string_view path,
               const std::vector<float>& samples,
               int sample_rate,
               int channels,
               int bits,
               bool dither)
{
    if (samples.empty()) return false;
    if (sample_rate <= 0 || channels <= 0) return false;
    if (samples.size() % static_cast<size_t>(channels) != 0) return false;  // 帧必须完整

    std::vector<uint8_t> wav;
    if (bits == 24)
        wav = encode_wav24(to_pcm24(samples, dither), sample_rate, channels);
    else
        wav = encode_wav(to_pcm16(samples, dither), sample_rate, channels);

    std::ofstream ofs(std::string(path), std::ios::binary);
    if (!ofs.is_open())
        return false;

    ofs.write(reinterpret_cast<const char*>(wav.data()),
              static_cast<std::streamsize>(wav.size()));
    return ofs.good();
}
