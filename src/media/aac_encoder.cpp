#include "rkcam/media/aac_encoder.hpp"

#include "rkcam/core/log.hpp"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}


#include <algorithm>
#include <cstring>
#include <memory>

namespace rkcam{

namespace{

static std::string ffmpegErrorToString(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

static int64_t channelLayoutForChannels(int channels)
{
    if(channels == 1)
    {
        return AV_CH_LAYOUT_MONO;
    }
    if(channels == 2)
    {
        return AV_CH_LAYOUT_STEREO;
    }

    return av_get_default_channel_layout(channels);
}

static AVSampleFormat chooseSampleFormat(
    const AVCodec* codec,
    AVSampleFormat preferred
)
{
    //AVSampleFormat（采样格式）：指的是 AV_SAMPLE_FMT_FLTP / AV_SAMPLE_FMT_S16
    if (!codec || !codec->sample_fmts) {
        return preferred;
    }

    for (const AVSampleFormat* p = codec->sample_fmts; *p != AV_SAMPLE_FMT_NONE; ++p)
    {
        if(*p == preferred)
        {
            return preferred;
        }
    }
    /*
     * 如果没有 FLTP，就用编码器支持的第一个格式。
     */
    return codec->sample_fmts[0];
}


static int aacSampleRateIndex(int sample_rate)
{
    switch (sample_rate) {
    case 96000: return 0;
    case 88200: return 1;
    case 64000: return 2;
    case 48000: return 3;
    case 44100: return 4;
    case 32000: return 5;
    case 24000: return 6;
    case 22050: return 7;
    case 16000: return 8;
    case 12000: return 9;
    case 11025: return 10;
    case 8000:  return 11;
    case 7350:  return 12;
    default:    return -1;
    }
}

/*
 * 构造 AAC-LC AudioSpecificConfig。
 *
 * bits:
 *   audioObjectType       5 bits, AAC-LC = 2
 *   samplingFrequencyIdx  4 bits
 *   channelConfiguration  4 bits
 */
static bool buildAacLcAudioSpecificConfig(
    int sample_rate,
    int channels,
    std::vector<uint8_t>& extradata
)
{
    extradata.clear();
    const int sample_index = aacSampleRateIndex(sample_rate);
    if (sample_index < 0) {
        return false;
    }

    if(channels <= 0 || channels > 7)
    {
        return false;
    }

    if(channels <= 0 || channels > 7)
    {
        return false;
    }

    const int audio_object_type = 2; //aac_lc
    const uint16_t asc = static_cast<uint16_t>(
        (audio_object_type << 11) |
        (sample_index << 7) |
        (channels << 3)  //最后三位是aac lc 的专属AOT Specific Config
    );

    extradata.push_back(static_cast<uint8_t>(asc >> 8) & 0xff);
    extradata.push_back(static_cast<uint8_t>(asc & 0xff));
    return true;
}

static int64_t audioSamplesToUs(
    int samples,
    int sample_rate
)
{
    if(samples <= 0 || sample_rate <= 0)
    {
        return 0;
    }

    return static_cast<int64_t>(samples) * 1000000LL /sample_rate;
}
static int16_t abs16(int16_t v)
{
    if (v == INT16_MIN) {
        return INT16_MAX;
    }
    return static_cast<int16_t>(v < 0 ? -v : v);
}

static int peakS16Mono(const int16_t* data, int samples)
{
    if (!data || samples <= 0) {
        return 0;
    }

    int peak = 0;
    for (int i = 0; i < samples; ++i) {
        peak = std::max(peak, static_cast<int>(abs16(data[i])));
    }

    return peak;
}

static float peakFltpMono(const AVFrame* frame)
{
    if (!frame || !frame->data[0] || frame->nb_samples <= 0) {
        return 0.0f;
    }

    const float* p = reinterpret_cast<const float*>(frame->data[0]);

    float peak = 0.0f;
    for (int i = 0; i < frame->nb_samples; ++i) {
        const float v = p[i] >= 0.0f ? p[i] : -p[i];
        if (v > peak) {
            peak = v;
        }
    }

    return peak;
}
} //namespace

AacEncoder::AacEncoder(const AacEncoderConfig& config)
    : config_(config)
{
}

AacEncoder::~AacEncoder()
{
    close();
}

bool AacEncoder::init()
{
    if(initialized_)
    {
        return true;
    }

    if(!validateConfig())
    {
        return false;
    }

    effective_input_channels_ = effectiveInputChannels();
    if (effective_input_channels_ <= 0) {
        RKCAM_LOGE("AacEncoder invalid effective input channels");
        return false;
    }

    if(!initCodec())
    {
        close();
        return false;
    }
    if(!initResampler())
    {
        close();
        return false;
    }

    if(!initFrameAndPacket())
    {
        close();
        return false;
    }

    initialized_ = true;
    flushed_ = false;
    next_output_pts_us_ = -1;
    RKCAM_LOGI("AacEncoder init success: input=%dHz/%dch/%s effective_in=%dch "
               "output=%dHz/%dch bitrate=%d frame_size=%d sample_fmt=%s extradata=%zu",
               config_.input_sample_rate,
               config_.input_channels,
               audioSampleFormatToString(config_.input_format),
               effective_input_channels_,
               config_.output_sample_rate,
               config_.output_channels,
               config_.bit_rate,
               frame_size_,
               codec_ctx_ && codec_ctx_->sample_fmt != AV_SAMPLE_FMT_NONE
                   ? av_get_sample_fmt_name(codec_ctx_->sample_fmt)
                   : "unknown",
               extradata_.size());
    return true;
}

bool AacEncoder::validateConfig() const 
{
    if (config_.stream_id.empty()) {
        RKCAM_LOGE("AacEncoder config error: stream_id is empty");
        return false;
    }

    if (config_.input_sample_rate <= 0 ||
        config_.output_sample_rate <= 0) {
        RKCAM_LOGE("AacEncoder config error: invalid sample_rate input=%d output=%d",
                   config_.input_sample_rate,
                   config_.output_sample_rate);
        return false;
    }

    if (config_.input_channels <= 0 ||
        config_.output_channels <= 0) {
        RKCAM_LOGE("AacEncoder config error: invalid channels input=%d output=%d",
                   config_.input_channels,
                   config_.output_channels);
        return false;
    }

    if (config_.output_channels != 1 &&
        config_.output_channels != 2) {
        RKCAM_LOGE("AacEncoder first version only supports output mono/stereo, got=%d",
                   config_.output_channels);
        return false;
    }

    if (config_.bit_rate <= 0) {
        RKCAM_LOGE("AacEncoder config error: invalid bit_rate=%d",
                   config_.bit_rate);
        return false;
    }

    if (config_.input_format != AudioSampleFormat::S16LE) {
        RKCAM_LOGE("AacEncoder first version only supports S16LE input, got=%d",
                   static_cast<int>(config_.input_format));
        return false;
    }

    if (config_.channel_select == AudioChannelSelect::LeftToMono ||
        config_.channel_select == AudioChannelSelect::RightToMono ||
        config_.channel_select == AudioChannelSelect::AverageToMono) {
        if (config_.input_channels < 2) {
            RKCAM_LOGE("AacEncoder channel select requires stereo input, input_channels=%d",
                       config_.input_channels);
            return false;
        }

        if (config_.output_channels != 1) {
            RKCAM_LOGE("AacEncoder Left/Right/AverageToMono requires output_channels=1, got=%d",
                       config_.output_channels);
            return false;
        }
    }

    return true; 
}

int AacEncoder::effectiveInputChannels() const
{
    switch (config_.channel_select) {
    case AudioChannelSelect::LeftToMono:
    case AudioChannelSelect::RightToMono:
    case AudioChannelSelect::AverageToMono:
        return 1;

    case AudioChannelSelect::Keep:
        return config_.input_channels;

    default:
        return 0;
    }
}

bool AacEncoder::initCodec()
{
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        RKCAM_LOGE("AacEncoder init failed: native AAC encoder not found");
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        RKCAM_LOGE("AacEncoder avcodec_alloc_context3 failed");
        return false;
    }

    codec_ctx_->codec_type = AVMEDIA_TYPE_AUDIO;
    codec_ctx_->codec_id = AV_CODEC_ID_AAC;

    codec_ctx_->sample_rate = config_.output_sample_rate;
    codec_ctx_->channels = config_.output_channels;
    codec_ctx_->channel_layout = channelLayoutForChannels(config_.output_channels);

    codec_ctx_->bit_rate = config_.bit_rate;
    codec_ctx_->sample_fmt = chooseSampleFormat(codec, AV_SAMPLE_FMT_FLTP);
    codec_ctx_->time_base = AVRational{1, config_.output_sample_rate};

    /*
     * AAC-LC。
     */
     codec_ctx_->profile = FF_PROFILE_AAC_LOW;

    /*
     * MP4/RTSP 后面需要 extradata。
     *
     */
    codec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    /*
     * 某些旧 FFmpeg 版本 native AAC 曾经需要 experimental。
     * 现在一般不会有影响。
     */
    codec_ctx_->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    int ret = avcodec_open2(codec_ctx_, codec, nullptr);


    frame_size_ = codec_ctx_->frame_size;
    if (frame_size_ <= 0) {
        RKCAM_LOGE("AacEncoder invalid codec frame_size=%d",
                   frame_size_);
        return false;
    }

    extradata_.clear();

    if(codec_ctx_->extradata && codec_ctx_->extradata_size > 0)
    {
        extradata_.assign(
            codec_ctx_->extradata, codec_ctx_->extradata + codec_ctx_->extradata_size
        );
    }
    else{
        /*
         * 兜底：手动构造 AAC-LC AudioSpecificConfig。
         */
        if(!buildAacLcAudioSpecificConfig(
            config_.output_sample_rate,
            config_.output_channels,
            extradata_
        ))
        {
            RKCAM_LOGE("AacEncoder failed to build fallback AAC extradata");
            return false;
        }
    }

    return true;
}

bool AacEncoder::initResampler()
{
    if(!codec_ctx_)
    {
        return false;
    }

    const int64_t in_layout = channelLayoutForChannels(effective_input_channels_);
    const int64_t out_layout = channelLayoutForChannels(config_.output_channels);

    swr_ctx_ = swr_alloc_set_opts(
        nullptr,                    // 1. 重采样上下文
        out_layout,                 // 2. 目标输出：声道布局
        codec_ctx_->sample_fmt,     // 3. 目标输出：采样格式
        config_.output_sample_rate, // 4. 目标输出：采样率
        in_layout,                  // 5. 输入原始：声道布局
        AV_SAMPLE_FMT_S16,          // 6. 输入原始：采样格式
        config_.input_sample_rate,  // 7. 输入原始：采样率
        0,                          // 8. 日志偏移量
        nullptr                     // 9. 日志上下文
    );

    if(!swr_ctx_)
    {
        RKCAM_LOGE("AacEncoder swr_alloc_set_opts failed");
        return false;
    }

    const int ret = swr_init(swr_ctx_);
    if (ret < 0) {
        RKCAM_LOGE("AacEncoder swr_init failed: %s",
                   ffmpegErrorToString(ret).c_str());
        return false;
    }

    return true;
}

bool AacEncoder::initFrameAndPacket()
{
    if(!codec_ctx_)
    {
        return false;
    }

    frame_ = av_frame_alloc();
    if (!frame_) {
        RKCAM_LOGE("AacEncoder av_frame_alloc failed");
        return false;
    }
    
    frame_->nb_samples = frame_size_;  //一帧采样点数
    frame_->format = codec_ctx_->sample_fmt;
    frame_->channel_layout =  codec_ctx_->sample_fmt;
    frame_->sample_rate = codec_ctx_->sample_rate;
    frame_->channels = codec_ctx_->channels;

    int ret = av_frame_get_buffer(frame_, 0);
    if (ret < 0) {
        RKCAM_LOGE("AacEncoder av_frame_get_buffer failed: %s",
                   ffmpegErrorToString(ret).c_str());
        return false;
    }

    avpkt_ = av_packet_alloc();
    if (!avpkt_) {
        RKCAM_LOGE("AacEncoder av_packet_alloc failed");
        return false;
    }

    return true;


}


bool AacEncoder::encode(
    const PipelineAudioFrame& frame,
    std::vector<EncodedPacket>& packets
)
{
    packets.clear();
    if(!initialized_)
    {
        RKCAM_LOGE("AacEncoder encode failed: not initialized");
        return false;
    }
    if(flushed_)
    {
        RKCAM_LOGE("AacEncoder encode failed: encoder already flushed");
        return false; 
    }

    if(!fillAudioFrame(frame))
    {
        return false;
    }

    const int ret = avcodec_send_frame(codec_ctx_, frame_);
    if (ret < 0) {
        RKCAM_LOGE("AacEncoder avcodec_send_frame failed: %s",
                   ffmpegErrorToString(ret).c_str());
        return false;
    }

    return receivePackets(packets);
}

bool AacEncoder::fillAudioFrame(const PipelineAudioFrame& input_frame)
{
    const uint8_t* input_data = nullptr;
    int input_channels = 0;
    int input_samples = 0;
    if(!prepareInputPcm(
        input_frame,
        input_data,
        input_channels,
        input_samples
    ))
    {
        return false;
    }
    if (input_samples != frame_size_) {
        RKCAM_LOGE("AacEncoder first version requires nb_samples=%d, got=%d",
                   frame_size_,
                   input_samples);
        return false;
    }
    const int ret_writable = av_frame_make_writable(frame_);
    if (ret_writable < 0) {
        RKCAM_LOGE("AacEncoder av_frame_make_writable failed: %s",
                   ffmpegErrorToString(ret_writable).c_str());
        return false;
    }

    const uint8_t* in_planes[1] = {
        input_data,
    };
    const int out_samples =swr_convert(
        swr_ctx_,       // 1. 刚才配好的规则上下文
        frame_->data,   // 2. [输出] 转换后的数据放哪里？（指向 AVFrame 内部的内存指针）
        frame_size_,    // 3. [输出] 输出缓冲区最多能装多少个采样点？（防止内存溢出，通常填 1024）
        in_planes,      // 4. [输入] 原始 PCM 数据存在哪里？（指向输入内存的指针）
        input_samples   // 5. [输入] 这次送进来的原始数据有多少个采样点？
    );
    if (out_samples < 0) {
        RKCAM_LOGE("AacEncoder swr_convert failed: %s",
                   ffmpegErrorToString(out_samples).c_str());
        return false;
    }
    if (out_samples != frame_size_) {
        RKCAM_LOGE("AacEncoder unexpected swr output samples=%d expected=%d",
                   out_samples,
                   frame_size_);
        return false;
    }

    // const float peak = peakFltpMono(frame_);
    // RKCAM_LOGI("after swr FLTP peak=%.6f nb_samples=%d",
    //            peak,
    //            frame_->nb_samples);

    frame_->nb_samples = out_samples;
    frame_->format = codec_ctx_->sample_fmt;
    frame_->channel_layout = codec_ctx_->channel_layout;
    frame_->sample_rate = codec_ctx_->sample_rate;
    frame_->channels = codec_ctx_->channels;

    /*
     * FFmpeg audio pts 使用 time_base = 1 / sample_rate。
     */
    frame_->pts = av_rescale_q(
        input_frame.pts_us,
        AVRational{1, 1000000},
        codec_ctx_->time_base
    );

    if(next_output_pts_us_ < 0)
    {
        next_output_pts_us_ = input_frame.pts_us;
    }
    return true;

}

bool AacEncoder::prepareInputPcm(
    const PipelineAudioFrame& frame,
    const uint8_t*& input_data,
    int& input_channels,
    int& input_samples
)
{
    input_data = nullptr;
    input_channels = 0;
    input_samples = 0;
    if (!frame.data() || frame.size() == 0) {
        RKCAM_LOGE("AacEncoder input frame is empty");
        return false;
    }

    if (frame.sample_rate != config_.input_sample_rate ||
        frame.channels != config_.input_channels ||
        frame.format != config_.input_format) {
        RKCAM_LOGE("AacEncoder input format mismatch: "
                   "frame=%dHz/%dch/%s config=%dHz/%dch/%s",
                   frame.sample_rate,
                   frame.channels,
                   audioSampleFormatToString(frame.format),
                   config_.input_sample_rate,
                   config_.input_channels,
                   audioSampleFormatToString(config_.input_format));
        return false;
    }

    if (frame.nb_samples <= 0) {
        RKCAM_LOGE("AacEncoder invalid input nb_samples=%d",
                   frame.nb_samples);
        return false;
    }

    const size_t expected_bytes = static_cast<size_t>(frame.nb_samples) * static_cast<size_t>(frame.channels) * sizeof(int16_t);

    if(frame.size() < expected_bytes){
        RKCAM_LOGE("AacEncoder input size too small: size=%zu expected=%zu",
                   frame.size(),
                   expected_bytes);
        return false;
    }

    input_samples = frame.nb_samples;
    // RKCAM_LOGI("frame.nb_samples %d", frame.nb_samples);
    if(config_.channel_select == AudioChannelSelect::Keep)
    {
        input_data = frame.data();
        input_channels = frame.channels;
        return true;
    }

    /*
     * S16_LE interleaved:
     *   L0 R0 L1 R1 ...
     */
    const int16_t*src = reinterpret_cast<const int16_t*>((frame.data()));

    mono_s16_buffer_.resize(static_cast<size_t>(frame.nb_samples));

    for(int i = 0; i < frame.nb_samples; i++)
    {
        const int16_t left = src[i * frame.channels + 0];
        const int16_t right = src[i * frame.channels + 1];

        int16_t out = 0;
        switch(config_.channel_select)
        {
            case AudioChannelSelect::LeftToMono:
                out = left;
                break;
            case AudioChannelSelect::RightToMono:
                out = right;
                break;
            case AudioChannelSelect::AverageToMono:{
                const int mixed = static_cast<int>(left) + static_cast<int>(right);
                out = static_cast<int16_t>(mixed / 2);
                break;
            }
            default:
                RKCAM_LOGE("AacEncoder unsupported channel_select=%d",
                        static_cast<int>(config_.channel_select));
                return false;
        }
        mono_s16_buffer_[static_cast<size_t>(i)] = out;
    }

    const int mono_peak =
        peakS16Mono(
            mono_s16_buffer_.data(),
            frame.nb_samples);

    // RKCAM_LOGI("AacEncoder channel select debug: mono_peak=%d select=%d",
    //            mono_peak,
    //            static_cast<int>(config_.channel_select)); 

    input_data = reinterpret_cast<const uint8_t*>(mono_s16_buffer_.data());
    input_channels = 1;

    return true;

}

bool AacEncoder::receivePackets(
    std::vector<EncodedPacket>& packets
)
{
    while(true)
    {
        const int ret = avcodec_receive_packet(codec_ctx_, avpkt_);
        if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            return true;
        }
        if (ret < 0) {
            RKCAM_LOGE("AacEncoder avcodec_receive_packet failed: %s",
                       ffmpegErrorToString(ret).c_str());
            return false;
        }
        
        EncodedPacket packet;
        packet.stream_id = config_.stream_id;
        packet.media_type = MediaType::Audio;
        packet.codec = CodecType::AAC;

        packet.buffer = std::make_shared<EncodedBuffer>();
        packet.buffer->data.assign(
            avpkt_->data,
            avpkt_->data + avpkt_->size
        );

        const int64_t duration_us = audioSamplesToUs(frame_size_, config_.output_sample_rate);
        if(avpkt_->pts != AV_NOPTS_VALUE)
        {
            packet.pts_us = av_rescale_q(
                avpkt_->pts,
                codec_ctx_->time_base,
                AVRational{1, 1000000}
            );
        }
        else{
            packet.pts_us = next_output_pts_us_ >= 0 ? next_output_pts_us_ : 0;
        }

        if(avpkt_->dts != AV_NOPTS_VALUE)
        {
            packet.dts_us = av_rescale_q(
                avpkt_->dts,
                codec_ctx_->time_base,
                AVRational{1, 1000000}
            );
        }
        else{
            packet.dts_us = packet.pts_us;
        }

        if(avpkt_->duration > 0)
        {
            packet.duration_us = av_rescale_q(
                avpkt_->duration,
                codec_ctx_->time_base,
                AVRational{1, 1000000}
            );
        }
        else{
            packet.duration_us = duration_us;
        }

        packet.key_frame = false;
        packet.eos = false;

        next_output_pts_us_ = packet.pts_us + packet.duration_us;

        packets.push_back(std::move(packet));

        av_packet_unref(avpkt_);
        
    }
}

bool AacEncoder::flush(std::vector<EncodedPacket>& packets)
{
    packets.clear();

    if(!initialized_)
    {
        return true;
    }

    if(flushed_)
    {
        return true;
    }

    const int ret = avcodec_send_frame(codec_ctx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) {
        RKCAM_LOGE("AacEncoder flush send null frame failed: %s",
                   ffmpegErrorToString(ret).c_str());
        return false;
    }

    flushed_ = true;
    return receivePackets(packets);
}

void AacEncoder::close()
{
    if(avpkt_)
    {
        av_packet_free(&avpkt_);
        avpkt_ = nullptr;
    }

    if(frame_)
    {
        av_frame_free(&frame_);
        frame_ = nullptr;
    }

    if(swr_ctx_)
    {
        swr_free(&swr_ctx_);
        swr_ctx_ = nullptr;
    }

    if(codec_ctx_)
    {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }


    extradata_.clear();
    mono_s16_buffer_.clear();

    initialized_ = false;
    flushed_ = false;
    frame_size_ = 0;
    effective_input_channels_ = 0;
    next_output_pts_us_ = -1;
}

bool AacEncoder::initialized() const
{
    return initialized_;
}

const std::vector<uint8_t>& AacEncoder::extradata() const
{
    return extradata_;
}

int AacEncoder::frameSize() const
{
    return frame_size_;
}

int AacEncoder::outputSampleRate() const
{
    return config_.output_sample_rate;
}

int AacEncoder::outputChannels() const
{
    return config_.output_channels;
}

int AacEncoder::bitRate() const
{
    return config_.bit_rate;
}

}