#include "wav_writer.h"

#include <fstream>
#include <cstdint>
#include <algorithm>
#include <stdexcept>

// WAV 文件头 (RIFF)
struct WavHeader {
    // RIFF 块
    char     riff_id[4]     = {'R', 'I', 'F', 'F'};
    uint32_t riff_size      = 0;     // 文件总长 - 8
    char     wave_id[4]     = {'W', 'A', 'V', 'E'};

    // fmt 子块
    char     fmt_id[4]      = {'f', 'm', 't', ' '};
    uint32_t fmt_size       = 16;    // PCM 头长
    uint16_t audio_format   = 1;     // 1 = PCM
    uint16_t num_channels   = 1;
    uint32_t sample_rate    = 44100;
    uint32_t byte_rate      = 0;     // sample_rate * channels * bytes_per_sample
    uint16_t block_align    = 0;     // channels * bytes_per_sample
    uint16_t bits_per_sample= 16;

    // data 子块
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

    // 头
    WavHeader hdr;
    hdr.num_channels  = static_cast<uint16_t>(channels);
    hdr.sample_rate   = static_cast<uint32_t>(sample_rate);
    hdr.bits_per_sample = 16;
    hdr.block_align   = static_cast<uint16_t>(channels * (hdr.bits_per_sample / 8));
    hdr.byte_rate     = hdr.sample_rate * hdr.block_align;
    hdr.data_size     = data_bytes;
    hdr.riff_size     = 36 + hdr.data_size;
    put(&hdr, sizeof(hdr));

    // 采样数据
    put(pcm.data(), pcm.size() * bytes_per_sample);
    return wav;
}

bool write_wav(std::string_view path,
               const std::vector<float>& samples,
               int sample_rate,
               int channels)
{
    if (samples.empty()) return false;
    if (sample_rate <= 0 || channels <= 0) return false;

    // float32 → int16 (clamp)
    std::vector<int16_t> pcm(samples.size());
    for (size_t i = 0; i < samples.size(); ++i) {
        float s = samples[i];
        if (s > 1.0f) s = 1.0f;
        else if (s < -1.0f) s = -1.0f;
        pcm[i] = static_cast<int16_t>(s * 32767.0f);
    }

    auto wav = encode_wav(pcm, sample_rate, channels);

    std::ofstream ofs(std::string(path), std::ios::binary);
    if (!ofs.is_open())
        return false;

    ofs.write(reinterpret_cast<const char*>(wav.data()),
              static_cast<std::streamsize>(wav.size()));
    return ofs.good();
}
