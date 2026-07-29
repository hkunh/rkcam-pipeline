#pragma once

#include "rkcam/audio/audio_source.hpp"
#include "rkcam/audio/audio_types.hpp"

#include <atomic>
#include <cstdint>
#include <string>

struct _snd_pcm;

namespace rkcam{

struct AlsaAudioSourceConfig{
    std::string device = "hw0,0";
    std::string stream_id = "audio0";

    /*
     * 根据你板子的 arecord 信息：
     *   FORMAT: S16_LE S24_LE S32_LE
     *   CHANNELS: [2 8]
     *   RATE: [8000 96000]
     *
     * 第一版建议：
     *   S16_LE / 48000 / 2ch
     */

    int sample_rate = 48000;
    int channels = 2;
    AudioSampleFormat format = AudioSampleFormat::S16LE;

    /*
     * AAC-LC 一帧通常是 1024 samples。
     * 所以 period_size 先用 1024，后面接 AAC 最舒服。
     */
    int period_size = 1024;
    int periods = 4;

    /*
     * 如果 readi 短读，是否允许直接输出短帧。
     * 第一版建议 false，尽量每次输出完整 period。
     */
    bool allow_partial_read = false;

};


class AlsaAudioSource : public IAudioSource{

public:
    explicit AlsaAudioSource(const AlsaAudioSourceConfig& config);
    ~AlsaAudioSource() override;

    AlsaAudioSource(const AlsaAudioSource&) = delete;
    AlsaAudioSource& operator=(const AlsaAudioSource&) = delete;

    bool open() override;
    bool start() override;
    bool readFrame(AudioSourceFrame& frame) override;
    void releaseFrame(AudioSourceFrame& frame) override;
    void stop() override;
    void close() override;

private:
    bool configureHwParams();
    bool configureSwParams();

    bool recoverFromReadError(int err);


    int bytesPerFrame() const;
    int frameBufferBytes(int nb_samples) const;

    static int64_t nowUs();
    bool getAlsaCapturePtsUs(int read_frames, int64_t& pte_us) const;
private:
    AlsaAudioSourceConfig config_;  //请求值

    _snd_pcm* handle_ = nullptr;

    int actual_sample_rate_ = 0; //真实值
    int actual_channels_ = 0;
    AudioSampleFormat actual_format_ = AudioSampleFormat::Unknown;
    int actual_period_size_ = 0;  //多少帧
    int actual_periods_ = 0;

    std::atomic<bool> opened_{false};
    std::atomic<bool> running_{false};

    int64_t frame_id_ = 0;


};

}//nampspace rkcam