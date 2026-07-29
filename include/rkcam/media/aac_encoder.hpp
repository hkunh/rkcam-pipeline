#pragma once

#include "rkcam/audio/audio_frame.hpp"
#include "rkcam/media/encoded_packet.hpp"

#include <cstdint>
#include <string>
#include <vector>


struct AVCodecContext;
struct SwrContext;
struct AVFrame;
struct AVPacket;

namespace rkcam{

enum class AudioChannelSelect{
    /*
     * 保持输入声道。
     * 例如 2ch -> 2ch。
     */
    Keep,

    /*
     * 取左声道转 mono。
     * 你的板子当前建议用这个。
     */
    LeftToMono,

    /*
     * 取右声道转 mono。
     */
    RightToMono,

    /*
     * 左右平均转 mono。
     * 当前不推荐，因为你右声道是静音，平均会让音量减半。
     */
    AverageToMono,

};


struct AacEncoderConfig{

    std::string stream_id = "audio0";
    /*
     * 输入 PCM 参数。
     * 你的 ALSA 当前是：
     *   S16_LE / 48000 / 2ch
     */
    int input_sample_rate = 48000;
    int input_channels = 2;
    AudioSampleFormat input_format = AudioSampleFormat::S16LE;

    /*
     * 输出 AAC 参数。
     * 第一版建议 mono AAC。
     */
    int output_sample_rate = 48000;
    int output_channels = 1;
    int bit_rate = 64000;

    /*
     * 当前你的有效麦克风在左声道。
     */
    AudioChannelSelect channel_select = AudioChannelSelect::LeftToMono;

};

class AacEncoder{
public:
    explicit AacEncoder(const AacEncoderConfig& config);
    ~AacEncoder();

    AacEncoder(const AacEncoder&) = delete;
    AacEncoder& operator=(const AacEncoder&) = delete;

    bool init();


    /*
     * 编码一帧 PCM。
     *
     * 注意：
     *   FFmpeg 编码器不保证每次 send_frame 都立刻 receive_packet。
     *   所以输出使用 vector。
     */
    bool encode(const PipelineAudioFrame& frame,
            std::vector<EncodedPacket>& packets);
    
    /*
     * stop 前调用，把编码器内部残留 packet 取出来。
     */
    bool flush(std::vector<EncodedPacket>& packets);

    void close();

    bool initialized() const;

    /*
     * AAC AudioSpecificConfig。
     * 后面 MP4 / RTSP 创建 audio stream 时需要。
     */
    const std::vector<uint8_t>& extradata() const;

    int frameSize() const;
    int outputSampleRate() const;
    int outputChannels() const;
    int bitRate() const;

private:
    bool validateConfig() const;

    bool initCodec();
    bool initResampler();
    bool initFrameAndPacket();

    bool prepareInputPcm(
        const PipelineAudioFrame& frame,
        const uint8_t*& input_data,
        int& input_channels,
        int& input_samples
    );

    bool fillAudioFrame(
        const PipelineAudioFrame& input_frame
    );

    bool receivePackets(std::vector<EncodedPacket>& packets);

    int effectiveInputChannels() const;


private:
    AacEncoderConfig config_;

    AVCodecContext* codec_ctx_ = nullptr;
    SwrContext* swr_ctx_ = nullptr; //Software Resample
    AVFrame* frame_ = nullptr;
    AVPacket* avpkt_ = nullptr;

    std::vector<uint8_t> extradata_;

    /*
     * LeftToMono / RightToMono / AverageToMono 时复用。
     */
    std::vector<int16_t> mono_s16_buffer_; //Monophonic

    bool initialized_ = false;
    bool flushed_ = false;


    // 注意，这里的frame_size_是指采样点数，单声道（Per Channel）在一帧里的采样点个数（即时间上的采样点数），而不是所有声道加起来的采样点总数，更不是字节数（Bytes）。
    int frame_size_ = 0;
    int effective_input_channels_ = 0;

    /*
     * 如果 FFmpeg packet 没给 pts，用这个兜底。
     */
    int64_t next_output_pts_us_ = -1;
};





}