#include "rkcam/pipeline/rtsp_push_stage.hpp"

#include "rkcam/core/log.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/buffer.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace rkcam {
namespace {

struct NalUnit{

    const uint8_t* data = nullptr;
    size_t size = 0;
};
struct AvPacketBufferHolder{

    /*
     * 用 shared_ptr<void> 保存 EncodedPacket::buffer 的生命周期。
     * 这样不需要在这里知道 EncodedBuffer 的具体类型。
     */
    std::shared_ptr<void> keep_alive;
};

static void avPacketBufferFreeCallback(void* opaque, uint8_t* data)
{
    //opaque:不透明指针
    (void)data; //防止编译器报错

    auto* holder = static_cast<AvPacketBufferHolder*>(opaque);
    delete holder;
}

static std::string ffmpegErrorToString(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

static void appendBe16(std::vector<uint8_t>& out, uint16_t value)
{
    //append 追加
    //Be Big-Endian
    //16代表处理的数据是16位
    out.push_back(static_cast<uint8_t>(value>>8 & 0xff));
    out.push_back(static_cast<uint8_t>(value & 0xff));
}

static const uint8_t* findStartCode(const uint8_t* begin, const uint8_t* end, size_t& start_code_len)
{
    start_code_len = 0;
    if (!begin || !end || begin >= end) {
        return nullptr;
    }

    for(const uint8_t* p = begin; p + 3 <=end; ++p)
    {
        if(p[0] == 0x00 &&
            p[1] == 0x00 &&
            p[2] == 0x01)
        {
            start_code_len = 3;
            return p ;
        }

        if(p + 4 <= end &&
            p[0] == 0x00 &&
            p[1] == 0x00 &&
            p[2] == 0x00 &&
            p[3] == 0x01)
        {
            start_code_len = 4;
            return p;
        }
    }
    return nullptr;
}

static bool splitAnnexBNals(const uint8_t* data, size_t size, std::vector<NalUnit>& nals)
{
    nals.clear();

    if (!data || size == 0) {
        return false;
    }

    const uint8_t* end = data + size;
    size_t sc_len = 0;
    const uint8_t* sc = findStartCode(data, end, sc_len);

    if (!sc) {
        return false;
    }
    while(sc){
        const uint8_t* nal_start = sc + sc_len;
        size_t next_sc_len = 0;
        const uint8_t* next_sc = findStartCode(nal_start, end, next_sc_len);
        const uint8_t* nal_end = next_sc ? next_sc : end;

        /*
         * 去掉 trailing zero。
         */
        while (nal_end > nal_start && *(nal_end - 1) == 0x00) {
            --nal_end;
        }

        if(nal_end > nal_start)
        {
            NalUnit nal;
            nal.data = nal_start;
            nal.size = static_cast<size_t>(nal_end - nal_start);
            nals.push_back(nal);
        }
        sc = next_sc;
        sc_len = next_sc_len;
    }
    return !nals.empty();

}

static bool extractSpsPpsFromAnnexB(const EncodedPacket& packet,
                                std::vector<uint8_t>& sps,
                                std::vector<uint8_t>& pps)
{
    sps.clear();
    pps.clear();
    std::vector<NalUnit> nals;

    if(!splitAnnexBNals(packet.data(), packet.size(), nals))
    {
        return false;
    }

    for(const auto& nal : nals)
    {
        if(!nal.data || nal.size == 0)
        {
            continue;
        }

        const uint8_t nal_type = nal.data[0] & 0x1f;

        /*
         * H264:
         *   7 = SPS
         *   8 = PPS
         */
        if(nal_type == 7 && sps.empty())
        {
            sps.assign(nal.data, nal.data + nal.size);
        }
        else if(nal_type == 8 && pps.empty())
        {
            pps.assign(nal.data, nal.data + nal.size);
        }

    }
    return !sps.empty() && !pps.empty();

}

static bool hasIdrNal(const EncodedPacket& packet)
{
    std::vector<NalUnit> nals;

    if (!splitAnnexBNals(packet.data(), packet.size(), nals)) {
        return false;
    }

    for (const auto& nal : nals) {
        if (!nal.data || nal.size == 0) {
            continue;
        }

        const uint8_t nal_type = nal.data[0] & 0x1f;

        /*
         * H264:
         *   5 = IDR
         */
        if (nal_type == 5) {
            return true;
        }
    }

    return false;
}
/**
 * 构造 H.264 的 AVCDecoderConfigurationRecord (AVCC 格式的 Extradata)。
 * 
 * 知识点：
 * 1. 这是一个非流式(Out-of-band)的全局配置记录，用于初始化解码器。
 * 2. 它是 MP4、FLV/RTMP、RTSP(用于SDP生成)等规范强制要求的初始化头。
 */
static bool buildAvccExtradata(const std::vector<uint8_t>& sps,
                               const std::vector<uint8_t>& pps,
                               std::vector<uint8_t>& extradata)
{
    extradata.clear();

    /*
     * SPS 至少需要：
     *   nal_header + profile_idc + constraint flags + level_idc
     */
    if (sps.size() < 4 || pps.empty()) {
        return false;
    }

    /*
     * 防缓冲区溢出安全边界检查：
     * 因为 AVCC 格式中，记录 SPS/PPS 长度的字段只有 2 字节(16 bits)，
     * 最大只能表示 2^16 - 1 = 65535 字节 (0xFFFF)。超过这个大小会导致长度字段数值溢出。
     */
    if (sps.size() > 0xffffu || pps.size() > 0xffffu) {
        return false;
    }

    // 1. [configurationVersion] - 占用 1 Byte
    // 配置版本号，目前国际标准固定为 0x01。
    extradata.push_back(0x01);

    // 2. [AVCProfileIndication] - 占用 1 Byte
    // H.264 配置文件/档次（如 Baseline=66, Main=77, High=100）。
    // 从 sps[1] 复制，用于告诉解码器当前视频需要哪种计算性能。
    extradata.push_back(sps[1]);

    // 3. [profile_compatibility] - 占用 1 Byte
    // 兼容性标志，从 sps[2] 复制，用于兼容低版本 profile。
    extradata.push_back(sps[2]);

    // 4. [AVCLevelIndication] - 占用 1 Byte
    // H.264 级别（如 Level 4.1=41, 5.1=51）。
    // 从 sps[3] 复制，用来限制解码器的最大分辨率、帧率及码率。
    extradata.push_back(sps[3]);

    /*
     * 5. [lengthSizeMinusOne] - 占用 1 Byte (前 6 bits 为保留位全置1，后 2 bits 为实际值)
     * 
     * 整套方案的“路标配置”，极其关键！它指示了后续媒体数据帧(NALUs)在传输时，
     * 前缀表示“帧长度”的字段到底占几个字节。
     * 计算公式：真实长度字节数 = lengthSizeMinusOne + 1。
     * 这里写入 0xFF (二进制 11111111)：
     *   - 前 6 位 111111 是保留位 (Reserved)。
     *   - 后 2 位 11 代表实际值 3，也就是 3 + 1 = 4 字节。
     * 说明后续视频帧（Length-Prefixed NALUs）均使用 4 字节的大端序数值来表示每帧的长度。
     */
    extradata.push_back(0xff);

    /*
     * 6. [numOfSequenceParameterSets] - 占用 1 Byte (前 3 bits 保留置1，后 5 bits 为数量)
     * 
     * 写入 0xE1 (二进制 11100001)：
     *   - 前 3 位 111 是保留位。
     *   - 后 5 位 00001 代表包含了 1 个 SPS。
     */
    extradata.push_back(0xe1);

    /*
     * 7. [SPS 长度 (sequenceParameterSetLength)] - 占用 2 Bytes (Big-Endian 大端序)
     * 写入 2 字节的大端序无符号数表示 SPS 的实际字节大小。
     */
    appendBe16(extradata, static_cast<uint16_t>(sps.size()));

    /*
     * 8. [SPS 实体数据 (sequenceParameterSetNALUnit)] - 变长
     * 写入真正的 SPS 原始字节数据。
     */
    extradata.insert(extradata.end(), sps.begin(), sps.end());

    /*
     * 9. [numOfPictureParameterSets] - 占用 1 Byte
     * 包含的 PPS 数量，这里写入 0x01，代表有 1 个 PPS。
     */
    extradata.push_back(0x01);

    /*
     * 10. [PPS 长度 (pictureParameterSetLength)] - 占用 2 Bytes (Big-Endian 大端序)
     * 写入 2 字节的大端序无符号数表示 PPS 的实际字节大小。
     */
    appendBe16(extradata, static_cast<uint16_t>(pps.size()));

    /*
     * 11. [PPS 实体数据 (pictureParameterSetNALUnit)] - 变长
     * 写入真正的 PPS 原始字节数据。
     */
    extradata.insert(extradata.end(), pps.begin(), pps.end());

    return true;
}


static AVCodecID ffmpegCodecIdFromCodec(CodecType codec)
{
    switch (codec) {
    case CodecType::H264:
        return AV_CODEC_ID_H264;
    case CodecType::AAC:
        return AV_CODEC_ID_AAC;
    default:
        return AV_CODEC_ID_NONE;
    }
}
}//namespace



RtspPushStage::RtspPushStage(
    const RtspPushStageConfig& config,
    BlockingQueue<EncodedPacket>& input_queue)
    : config_(config),
      input_queue_(input_queue)
{
}

RtspPushStage::~RtspPushStage()
{
    stop();
}


bool RtspPushStage::start()
{
    if (running_) {
        return true;
    }

    if (config_.url.empty()) {
        RKCAM_LOGE("[%s] start failed: url is empty",
                   config_.stage_name.c_str());
        return false;
    }

    if (!config_.video.enabled) {
        RKCAM_LOGE("[%s] start failed: video must be enabled in first version",
                   config_.stage_name.c_str());
        return false;
    }

    if (config_.video.codec != CodecType::H264) {
        RKCAM_LOGE("[%s] start failed: only H264 video is supported now",
                   config_.stage_name.c_str());
        return false;
    }

    if (config_.video.width <= 0 ||
        config_.video.height <= 0 ||
        config_.video.fps <= 0) {
        RKCAM_LOGE("[%s] start failed: invalid video config %dx%d fps=%d",
                   config_.stage_name.c_str(),
                   config_.video.width,
                   config_.video.height,
                   config_.video.fps);
        return false;
    }

    if (config_.audio.enabled) {
        if (config_.audio.codec != CodecType::AAC) {
            RKCAM_LOGE("[%s] start failed: only AAC audio is reserved now",
                       config_.stage_name.c_str());
            return false;
        }

        if (config_.audio.sample_rate <= 0 ||
            config_.audio.channels <= 0) {
            RKCAM_LOGE("[%s] start failed: invalid audio config sample_rate=%d channels=%d",
                       config_.stage_name.c_str(),
                       config_.audio.sample_rate,
                       config_.audio.channels);
            return false;
        }

        /*
         * AAC RTSP/RTP 通常需要 AudioSpecificConfig 生成 SDP。
         * 后面你接 AAC encoder 时，把 encoder extradata 填进来。
         */
        if (config_.audio.extradata.empty()) {
            RKCAM_LOGE("[%s] start failed: AAC audio enabled but extradata is empty",
                       config_.stage_name.c_str());
            return false;
        }
    }

    input_packets_ = 0;
    pushed_packets_ = 0;
    dropped_packets_ = 0;
    write_failures_ = 0;

    first_pts_us_ = -1;
    last_video_dts_ = INT64_MIN;
    last_audio_dts_ = INT64_MIN;
    header_written_ = false;

    running_ = true;
    thread_ = std::thread(&RtspPushStage::threadLoop, this);

    RKCAM_LOGI("[%s] RtspPushStage started: url=%s video=%dx%d fps=%d audio=%d write_mode=%d",
               config_.stage_name.c_str(),
               config_.url.c_str(),
               config_.video.width,
               config_.video.height,
               config_.video.fps,
               config_.audio.enabled ? 1 : 0,
               static_cast<int>(config_.packet_write_mode));

    return true;

}

void RtspPushStage::stop()
{

    if(!running_ && !thread_.joinable())
    {
        return;
    }

    input_queue_.stop();
    if(thread_.joinable())
    {
        thread_.join();
    }
    closeMuxer();

    running_ = false;

    RKCAM_LOGI("[%s] RtspPushStage stopped, input_packets=%d pushed_packets=%d dropped_packets=%d write_failures=%d",
               config_.stage_name.c_str(),
               input_packets_,
               pushed_packets_,
               dropped_packets_,
               write_failures_);

}



void RtspPushStage::threadLoop()
{
    while (true) {
        EncodedPacket packet;

        if (!input_queue_.pop(packet)) {
            if (input_queue_.stopped()) {
                break;
            }

            continue;
        }

        ++input_packets_;

        if (packet.empty()) {
            ++dropped_packets_;
            continue;
        }
        /*
         * 第一版：
         *   RTSP muxer 从第一个带 SPS/PPS 的视频关键帧初始化。
         *
         * 后面接音频时：
         *   audio.enabled=true 时，audio stream 会在同一个 header 里创建。
         *   在 header_written_ 前到来的音频 packet 暂时丢弃。
         */


        if(!header_written_){
            if(packet.media_type != MediaType::Video || packet.codec != CodecType::H264)
            {
                ++dropped_packets_;
                continue;
            }

            const bool is_key = packet.key_frame || hasIdrNal(packet);

            if(config_.video.wait_key_frame && !is_key)
            {
                ++dropped_packets_;
                continue;
            }

            if(!initMuxerFromVideoPacket(packet))
            {
                ++dropped_packets_;
                continue;
            }
        }

        if(!writePacket(packet))
        {
            ++write_failures_;

            if (config_.max_write_failures > 0 &&
                write_failures_ >= config_.max_write_failures) {
                RKCAM_LOGE("[%s] too many write failures, exit thread",
                           config_.stage_name.c_str());
                break;
            }

            continue;
        }
        ++pushed_packets_;

        if (config_.log_interval > 0 &&
            pushed_packets_ % config_.log_interval == 0) {
            RKCAM_LOGI("[%s] pushed_packets=%d stream=%s media=%d codec=%d pts=%lld size=%zu key=%d",
                       config_.stage_name.c_str(),
                       pushed_packets_,
                       packet.stream_id.c_str(),
                       static_cast<int>(packet.media_type),
                       static_cast<int>(packet.codec),
                       static_cast<long long>(packet.pts_us),
                       packet.size(),
                       packet.key_frame ? 1 : 0);
        }
    }

    running_ = false;

    RKCAM_LOGI("[%s] RtspPushStage thread exit, input_packets=%d pushed_packets=%d dropped_packets=%d write_failures=%d",
               config_.stage_name.c_str(),
               input_packets_,
               pushed_packets_,
               dropped_packets_,
               write_failures_);
}

bool RtspPushStage::initMuxerFromVideoPacket(const EncodedPacket& packet)
{
    if(header_written_)
    {
        return true;
    }
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;

    if(!extractSpsPpsFromAnnexB(packet, sps, pps))
    {
        RKCAM_LOGE("[%s] wait H264 SPS/PPS packet, pts=%lld size=%zu",
                   config_.stage_name.c_str(),
                   static_cast<long long>(packet.pts_us),
                   packet.size());
        return false;
    }

    int ret = avformat_alloc_output_context2(&fmt_ctx_, nullptr, "rtsp", config_.url.c_str());
    if (ret < 0 || !fmt_ctx_) {
        RKCAM_LOGE("[%s] avformat_alloc_output_context2 failed: %s",
                   config_.stage_name.c_str(),
                   ffmpegErrorToString(ret).c_str());
        return false;
    }
    if(!createVideoStreamFromH264(sps, pps))
    {
        RKCAM_LOGE("[%s] createVideoStreamFromH264 failed",
                   config_.stage_name.c_str());
        closeMuxer();
        return false;
    }

    if (config_.audio.enabled) {
        if (!createAudioStream()) {
            RKCAM_LOGE("[%s] createAudioStream failed",
                       config_.stage_name.c_str());
            closeMuxer();
            return false;
        }
    }

    AVDictionary* options = nullptr;
    if(config_.rtsp_over_tcp)
    {
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
    }
    /*
     * 降低 muxer 缓冲倾向。
     * 不同 FFmpeg 版本对这些选项支持略有差异。
     */
    av_dict_set(&options, "muxdelay", "0", 0);
    av_dict_set(&options, "fflags", "nobuffer", 0);

    ret = avformat_write_header(fmt_ctx_, &options);
    av_dict_free(&options);
    if (ret < 0) {
        RKCAM_LOGE("[%s] avformat_write_header failed: %s url=%s",
                   config_.stage_name.c_str(),
                   ffmpegErrorToString(ret).c_str(),
                   config_.url.c_str());
        closeMuxer();
        return false;
    }

    first_pts_us_ = packet.pts_us >= 0 ? packet.pts_us : 0;
    header_written_ = true;
    RKCAM_LOGI("[%s] RTSP muxer initialized: url=%s video=%dx%d fps=%d audio=%d",
               config_.stage_name.c_str(),
               config_.url.c_str(),
               config_.video.width,
               config_.video.height,
               config_.video.fps,
               config_.audio.enabled ? 1 : 0);

    return true;
}



bool RtspPushStage::createVideoStreamFromH264(
    const std::vector<uint8_t>& sps,
    const std::vector<uint8_t>& pps)
{
    if (!fmt_ctx_) {
        return false;
    }
    std::vector<uint8_t> extradata;
    if(!buildAvccExtradata(sps, pps, extradata))
    {
        RKCAM_LOGE("[%s] buildAvccExtradata failed",
                   config_.stage_name.c_str());
        return false;
    }

    video_stream_ = avformat_new_stream(fmt_ctx_, nullptr);
    if (!video_stream_) {
        RKCAM_LOGE("[%s] avformat_new_stream video failed",
                   config_.stage_name.c_str());
        return false;
    }

    /*
     * RTP H264 常用 90kHz clock。
     */
    // r_frame_rate 的本质是 “用来渲染和解码的参考帧率”：
    // 你写视频（Muxing）时：你必须主动设值。为了防止变帧率视频在流畅阶段卡顿，你要把它的性能上限（最大帧率）填进去。
    // 你读视频（Demuxing）时：由 FFmpeg 自动计算并填值。你直接去读它，拿到的就是当前网络或文件里算出来的实际实时帧率。
    video_stream_->time_base = AVRational{1, 90000};
    video_stream_->avg_frame_rate = AVRational{config_.video.fps, 1};
    video_stream_->r_frame_rate = AVRational{config_.video.fps, 1};

    AVCodecParameters* par = video_stream_->codecpar;
    par->codec_type = AVMEDIA_TYPE_VIDEO;
    par->codec_id = AV_CODEC_ID_H264;
    par->width = config_.video.width;
    par->height = config_.video.height;
    par->codec_tag = 0;
    par->format = AV_PIX_FMT_NONE;

    par->extradata = static_cast<uint8_t*>(av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!par->extradata) {
        RKCAM_LOGE("[%s] av_mallocz video extradata failed",
                   config_.stage_name.c_str());
        return false;
    }
    std::memcpy(par->extradata, extradata.data(), extradata.size());
    par->extradata_size = static_cast<int>(extradata.size());

    return true;


}


bool RtspPushStage::createAudioStream()
{
    if(!fmt_ctx_)
    {
        return false;
    }
    
    const AVCodecID codec_id = ffmpegCodecIdFromCodec(config_.audio.codec);
    if (codec_id == AV_CODEC_ID_NONE) {
        RKCAM_LOGE("[%s] unsupported audio codec=%d",
                   config_.stage_name.c_str(),
                   static_cast<int>(config_.audio.codec));
        return false;
    }

    audio_stream_ = avformat_new_stream(fmt_ctx_, nullptr);
    if (!audio_stream_) {
        RKCAM_LOGE("[%s] avformat_new_stream audio failed",
                   config_.stage_name.c_str());
        return false;
    }
    audio_stream_->time_base = AVRational{1, config_.audio.sample_rate};

    AVCodecParameters* par = audio_stream_->codecpar;
    par->codec_type = AVMEDIA_TYPE_AUDIO;
    par->codec_id = codec_id;
    par->codec_tag = 0;
    par->sample_rate = config_.audio.sample_rate;
    par->channels = config_.audio.channels;
    par->channel_layout = config_.audio.channels == 1 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
    par->bit_rate = config_.audio.bit_rate;

    if(!config_.audio.extradata.empty())
    {
        par->extradata = static_cast<uint8_t*>(av_mallocz(config_.audio.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!par->extradata) {
            RKCAM_LOGE("[%s] av_mallocz audio extradata failed",
                       config_.stage_name.c_str());
            return false;
        }
        std::memcpy(par->extradata, config_.audio.extradata.data(), config_.audio.extradata.size());
        par->extradata_size = static_cast<int>(config_.audio.extradata.size());
    }
    return true;
}

bool RtspPushStage::writePacket(const EncodedPacket& packet)
{
    if (!header_written_ || !fmt_ctx_) {
        return false;
    }

    if (packet.media_type == MediaType::Video) {
        return writeVideoPacket(packet);
    }

    if (packet.media_type == MediaType::Audio) {
        return writeAudioPacket(packet);
    }

    ++dropped_packets_;
    return true;
}

bool RtspPushStage::writeVideoPacket(const EncodedPacket& packet)
{
    if (!video_stream_) {
        return false;
    }

    if (packet.codec != CodecType::H264) {
        RKCAM_LOGE("[%s] unsupported video codec=%d",
                   config_.stage_name.c_str(),
                   static_cast<int>(packet.codec));
        return false;
    }

    const bool is_key =
        packet.key_frame || hasIdrNal(packet);

    return writeAvPacket(packet, video_stream_, is_key);
}

bool RtspPushStage::writeAudioPacket(const EncodedPacket& packet)
{
    if (!config_.audio.enabled) {
        /*
         * 当前没有启用音频 stream，直接丢弃音频 packet。
         */
        ++dropped_packets_;
        return true;
    }

    if (!audio_stream_) {
        return false;
    }

    if (packet.codec != config_.audio.codec) {
        RKCAM_LOGE("[%s] audio codec mismatch packet=%d config=%d",
                   config_.stage_name.c_str(),
                   static_cast<int>(packet.codec),
                   static_cast<int>(config_.audio.codec));
        return false;
    }

    return writeAvPacket(packet, audio_stream_, false);
}

bool RtspPushStage::writeAvPacket(
    const EncodedPacket& packet,
    AVStream* stream,
    bool is_key)
{
    if (!fmt_ctx_ || !stream || packet.empty()) {
        return false;
    }
    if (packet.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        RKCAM_LOGE("[%s] packet too large: size=%zu",
                   config_.stage_name.c_str(),
                   packet.size());
        return false;
    }

    AVPacket avpkt;
    av_init_packet(&avpkt);

    bool ok = false;

    if(config_.packet_write_mode == RtspPacketWriteMode::Copy)
    {
        ok = makeAvPacketCopy(packet, avpkt);
    }
    else{
        ok = makeAvPacketRefCountedNoCopy(packet, avpkt);
    }

    if (!ok) {
        RKCAM_LOGE("[%s] make AVPacket failed, media=%d pts=%lld size=%zu",
                   config_.stage_name.c_str(),
                   static_cast<int>(packet.media_type),
                   static_cast<long long>(packet.pts_us),
                   packet.size());
        return false;
    }

    avpkt.stream_index = stream->index;
    avpkt.pos = -1;

    const AVRational input_tb = AVRational{1, 1000000};
    
    int64_t pts_us = packet.pts_us;
    int64_t dts_us = packet.dts_us;
    if (pts_us < 0) {
        pts_us = 0;
    }
    if (dts_us < 0) {
        dts_us = pts_us;
    }

    const int64_t rel_pts_us = relativePtsUs(pts_us);
    const int64_t rel_dts_us = relativePtsUs(dts_us);

    avpkt.pts = av_rescale_q(rel_pts_us, input_tb, stream->time_base);
    avpkt.dts = av_rescale_q(rel_dts_us, input_tb, stream->time_base);
    int64_t duration_us = packet.duration_us;
    if(duration_us <= 0 && packet.media_type == MediaType::Video)
    {
        duration_us = 1000000LL / std::max(1, config_.video.fps);
    }
    if (duration_us > 0) {
        avpkt.duration = av_rescale_q(
            duration_us,
            input_tb,
            stream->time_base);
    } else {
        avpkt.duration = 0;
    }

    /*
     * 防御：保证每个 stream 内 DTS 单调递增。
     * 当前 MPP H264 无 B 帧，正常应该 dts == pts。
     */
    int64_t* last_dts = nullptr;
    if(packet.media_type == MediaType::Video)
    {
        last_dts = &last_video_dts_;
    }
    else if(packet.media_type == MediaType::Audio)
    {
        last_dts = &last_audio_dts_;
    }
    if(last_dts)
    {
        if(*last_dts != INT64_MIN && avpkt.dts <= *last_dts)
        {
            avpkt.dts = *last_dts + 1;
            if(avpkt.pts < avpkt.dts)
            {
                avpkt.pts = avpkt.dts;
            }
        }
        *last_dts = avpkt.dts;
    }

    if(is_key)
    {
        avpkt.flags |= AV_PKT_FLAG_KEY;
    }

    const int ret = av_interleaved_write_frame(fmt_ctx_, &avpkt);
    if(ret < 0)
    {
        /*
         * 失败时需要释放 AVPacket。
         * 成功时 av_interleaved_write_frame 会消费 packet。
         */
        av_packet_unref(&avpkt);
        RKCAM_LOGE("[%s] av_interleaved_write_frame failed: %s media=%d pts=%lld size=%zu",
                   config_.stage_name.c_str(),
                   ffmpegErrorToString(ret).c_str(),
                   static_cast<int>(packet.media_type),
                   static_cast<long long>(packet.pts_us),
                   packet.size());
        return false;
    }
    return true;

}

bool RtspPushStage::makeAvPacketCopy(const EncodedPacket& packet, AVPacket& avpkt)
{
    av_init_packet(&avpkt);
    if (packet.empty()) {
        return false;
    }

    const int packet_size = static_cast<int>(packet.size());

    const int ret = av_new_packet(&avpkt, packet_size);
    if (ret < 0) {
        RKCAM_LOGE("[%s] av_new_packet failed: %s",
                   config_.stage_name.c_str(),
                   ffmpegErrorToString(ret).c_str());
        return false;
    }

    std::memcpy(avpkt.data, packet.data(), packet.size());
    return true;
}
bool RtspPushStage::makeAvPacketRefCountedNoCopy(const EncodedPacket& packet, AVPacket& avpkt)
{
    av_init_packet(&avpkt);
    if (packet.empty()) {
        return false;
    }

    /*
     * 这个模式要求 EncodedPacket::buffer 是 shared_ptr。
     * 如果你的 EncodedPacket 没有 buffer，说明无法保证生命周期，
     * 这里直接失败，避免裸指针悬空。
     */
    if (!packet.buffer) {
        RKCAM_LOGE("[%s] no-copy mode requires packet.buffer shared_ptr",
                   config_.stage_name.c_str());
        return false;
    }

    const int packet_size = static_cast<int>(packet.size());

    auto* holder = new AvPacketBufferHolder;
    holder->keep_alive = packet.buffer;

    avpkt.data = const_cast<uint8_t*>(packet.data());
    avpkt.size = packet_size;

    avpkt.buf = av_buffer_create(avpkt.data, avpkt.size, avPacketBufferFreeCallback, holder, 0);
    if (!avpkt.buf) {
        delete holder;
        avpkt.data = nullptr;
        avpkt.size = 0;
        return false;
    }

    return true;


}

int64_t RtspPushStage::relativePtsUs(int64_t pts_us) const
{
    if (first_pts_us_ < 0) {
        return pts_us;
    }

    if (pts_us < first_pts_us_) {
        return 0;
    }

    return pts_us - first_pts_us_;
}
void RtspPushStage::closeMuxer()
{
    if (!fmt_ctx_) {
        return;
    }
    if(header_written_)
    {
        const int ret = av_write_trailer(fmt_ctx_);
        if (ret < 0) {
            RKCAM_LOGE("[%s] av_write_trailer failed: %s",
                       config_.stage_name.c_str(),
                       ffmpegErrorToString(ret).c_str());
        }
    }

    avformat_free_context(fmt_ctx_);
    fmt_ctx_ = nullptr;
    video_stream_ = nullptr;
    audio_stream_ = nullptr;

    header_written_ = false;
    first_pts_us_ = -1;

    last_video_dts_ = INT64_MIN;
    last_audio_dts_ = INT64_MIN;
}

}//namespace rkcam 