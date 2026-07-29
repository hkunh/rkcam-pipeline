#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace rkcam {

enum class AudioSampleFormat {
    Unknown,
    S16LE,
    S24LE,
    S32LE,
    Float32Planar,
};

inline const char* audioSampleFormatToString(AudioSampleFormat format)
{
    switch (format) {
    case AudioSampleFormat::S16LE:
        return "S16_LE";
    case AudioSampleFormat::S24LE:
        return "S24_LE";
    case AudioSampleFormat::S32LE:
        return "S32_LE";
    case AudioSampleFormat::Float32Planar:
        return "FLTP";
    default:
        return "Unknown";
    }
}

/*
 * 这里返回每个 sample 的物理占用字节数。
 *
 * 注意：
 *   ALSA 的 S24_LE 通常是 24-bit valid in 32-bit container，
 *   物理占用一般按 4 bytes 处理。
 */
inline int audioSampleFormatBytes(AudioSampleFormat format)
{
    switch (format) {
    case AudioSampleFormat::S16LE:
        return 2;
    case AudioSampleFormat::S24LE:
        return 4;
    case AudioSampleFormat::S32LE:
        return 4;
    case AudioSampleFormat::Float32Planar:
        return 4;
    default:
        return 0;
    }
}

inline int audioFrameBytes(AudioSampleFormat format, int channels, int nb_samples)
{
    const int bytes_per_sample = audioSampleFormatBytes(format);
    if(bytes_per_sample <= 0 || channels <=0 || nb_samples <=0)
    {
        return 0;
    }
    return bytes_per_sample * channels * nb_samples;
}

inline int64_t audioDurationUs(int nb_samples, int sample_rate)
{
    if(nb_samples <=0 || sample_rate <=0)
    {
        return 0;
    }
    return static_cast<int64_t>(nb_samples) * 1000000LL / sample_rate;
}

} //namespace rkcam