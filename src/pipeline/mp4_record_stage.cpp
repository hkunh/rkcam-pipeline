#include "rkcam/pipeline/mp4_record_stage.hpp"
#include "rkcam/core/log.hpp"


extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace rkcam{
namespace{

//NALU（Network Abstraction Layer Unit，网络提取单元）
struct NalUnit{
    const uint8_t* data = nullptr; //纯数据起始地址
    size_t size = 0; // 💡 注意：纯数据的长度，不包含起始码的 4 字节！
};
    
static std::string ffmpegErrorToString(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] {};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

void appendStartCode4(std::vector<uint8_t>& out)
{
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x01);
}

/*
 * 只用于初始化阶段提取 SPS/PPS。
 * 不在每帧写入时调用。
 */
 const uint8_t* findStartCode(const uint8_t* begin, const uint8_t* end, size_t& start_code_len)
 {
    start_code_len = 0;//返回起起始码长度，用于判断是3字节起始码还是四字节起始码
    if (!begin || !end || begin >= end) {
        return nullptr;
    }
    for(const uint8_t* p = begin; p + 3 <= end; p++)
    {
        /*
         * 00 00 01
         */
        if(p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x01)
        {
            start_code_len = 3;
            return p;
        }
        /*
         * 00 00 00 01
         */
        if(p + 4 <= end && p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x00 && p[3] == 0x01)
        {
            start_code_len = 4;
            return p;
        }
    }
    return nullptr;

 }

/*
* 只用于初始化阶段提取 SPS/PPS。
*/
bool splitAnnexBNals(const uint8_t* data, size_t size, std::vector<NalUnit>& nals)
{
    nals.clear();
    if(!data || size==0)
    {
        return false;
    }
    const uint8_t* end = data + size;

    size_t sc_len = 0;
    const uint8_t* sc = findStartCode(data, end, sc_len);

    if(!sc)
    {
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
        while(nal_end > nal_start && *(nal_end -1) == 0x00)
        {
            nal_end--;
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
/*
 * 从 MPP 输出的 Annex-B H264 packet 中提取 SPS/PPS。
 *
 * 注意：
 *   MPP 输出的 packet 本身已经有起始码。
 *   这里不是给 packet 加起始码。
 *   这里只是拿出 SPS/PPS，给 FFmpeg codecpar->extradata 用。
 */

bool extractSpsPpsFromAnnexB(
    const std::vector<uint8_t>& data,
    std::vector<uint8_t>& sps,
    std::vector<uint8_t>& pps)
{
    sps.clear();
    pps.clear();
    std::vector<NalUnit> nals;

    if(!splitAnnexBNals(data.data(), data.size(), nals)){
        return false;
    }

    for(const auto& nal : nals)
    {
        if (!nal.data || nal.size == 0) {
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

/*
 * 构造 Annex-B extradata:
 *
 *   00 00 00 01 SPS 00 00 00 01 PPS
 *
 * 注意：
 *   这是给 FFmpeg AVCodecParameters::extradata 的。
 *   不是修改 MPP 输出的 packet。
 */
 bool buildAnnexBExtradata(const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps, std::vector<uint8_t>& extradata)
 {
    extradata.clear();
    if(sps.empty() || pps.empty())
    {
        return false;
    }
    appendStartCode4(extradata);
    extradata.insert(extradata.end(), sps.begin(), sps.end());

    appendStartCode4(extradata);
    extradata.insert(extradata.end(), pps.begin(), pps.end());

    return true;
 }


}

Mp4RecordStage::Mp4RecordStage(
    const Mp4RecordStageConfig& config,
    BlockingQueue<EncodedPacket>& input_queue)
    : config_(config),
      input_queue_(input_queue)
{
}

Mp4RecordStage::~Mp4RecordStage()
{
    stop();
}

bool Mp4RecordStage::start()
{
    if(running_)
    {
        return true;
    }
    if(config_.output_path.empty())
    {
        RKCAM_LOGE("[%s] output_path is empty",
                   config_.stage_name.c_str());
        return false;
    }

    if (config_.width <= 0 ||
        config_.height <= 0 ||
        config_.fps <= 0) {
        RKCAM_LOGE("[%s] invalid config: width=%d height=%d fps=%d",
                   config_.stage_name.c_str(),
                   config_.width,
                   config_.height,
                   config_.fps);
        return false;
    }

    if (config_.codec != CodecType::H264) {
        RKCAM_LOGE("[%s] only H264 is supported now, codec=%d",
                   config_.stage_name.c_str(),
                   static_cast<int>(config_.codec));
        return false;
    }

    saved_packets_ = 0;
    failed_packets_ = 0;
    first_pts_us_ = -1;
    header_written_ = false;

    running_ = true;
    thread_ = std::thread(&Mp4RecordStage::threadLoop, this);

    RKCAM_LOGI("[%s] Mp4RecordStage started, output=%s",
               config_.stage_name.c_str(),
               config_.output_path.c_str());

    return true;
}

void Mp4RecordStage::stop()
{
    if(!running_ && !thread_.joinable())
    {
        return;
    }

    /*
    * drain stop:
    * input_queue_.stop() 不会丢弃已有 EncodedPacket。
    */
    input_queue_.stop();
    if(thread_.joinable())
    {
        thread_.join();
    }
    closeMuxer();

    running_ = false;
    RKCAM_LOGI("[%s] Mp4RecordStage stopped, saved_packets=%d failed_packets=%d",
            config_.stage_name.c_str(),
            saved_packets_,
            failed_packets_);

}

void Mp4RecordStage::threadLoop()
{
    while(true)
    {
        EncodedPacket packet;
        if(!input_queue_.pop(packet))
        {
            if(input_queue_.stopped())
            {
                break;
            }
            continue;
        }

        if (packet.media_type != MediaType::Video ||
            packet.codec != CodecType::H264) {
            RKCAM_LOGE("[%s] unsupported packet: media=%d codec=%d",
                       config_.stage_name.c_str(),
                       static_cast<int>(packet.media_type),
                       static_cast<int>(packet.codec));
            ++failed_packets_;
            continue;
        }
        if (packet.empty()) {
            continue;
        }

        /*
        * 第一版：
        * 等到第一个带 SPS/PPS 的 packet，再初始化 MP4 muxer。
        *
        * MPP 一般会在第一个 IDR packet 前带 SPS/PPS。
        */
        if(!header_written_)
        {
            if(!initMuxerFromPacket(packet))
            {
                RKCAM_LOGE("[%s] initMuxerFromPacket failed, wait next packet, pts=%lld size=%zu",
                           config_.stage_name.c_str(),
                           static_cast<long long>(packet.pts_us),
                           packet.size());
                continue;
            }
        }

        if(!writePacket(packet))
        {
            RKCAM_LOGE("[%s] writePacket failed, pts=%lld size=%zu",
                       config_.stage_name.c_str(),
                       static_cast<long long>(packet.pts_us),
                       packet.size());
            ++failed_packets_;
            continue;
        }

        ++saved_packets_;
        if (saved_packets_ % 30 == 0) {
            RKCAM_LOGI("[%s] saved_packets=%d pts=%lld size=%zu key=%d",
                       config_.stage_name.c_str(),
                       saved_packets_,
                       static_cast<long long>(packet.pts_us),
                       packet.size(),
                       packet.key_frame ? 1 : 0);
        }
    }
    
    running_ = false;
    RKCAM_LOGI("[%s] Mp4RecordStage thread exit, saved_packets=%d failed_packets=%d",
            config_.stage_name.c_str(),
            saved_packets_,
            failed_packets_);
}

bool Mp4RecordStage::initMuxerFromPacket(const EncodedPacket& packet)
{
    if(header_written_)
    {
        return true;
    }

    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;

    if(!extractSpsPpsFromAnnexB(packet.buffer->data, sps, pps))
    {
        /*
         * 当前 packet 没带 SPS/PPS。
         * 不是致命错误，等后面的 packet。
         */
        return false;
    }

    std::vector<uint8_t> extradata;
    if(!buildAnnexBExtradata(sps, pps, extradata))
    {
        RKCAM_LOGE("[%s] build Annex-B extradata failed",
                   config_.stage_name.c_str());
        return false;
    }

    // int avformat_alloc_output_context2(
    // AVFormatContext **ctx,
    // const AVOutputFormat *oformat,
    // const char *format_name,
    // const char *filename
    // );
    int ret = avformat_alloc_output_context2(
        &fmt_ctx_,
        nullptr,
        "mp4",
        config_.output_path.c_str());
    if (ret < 0 || !fmt_ctx_) {
        RKCAM_LOGE("[%s] avformat_alloc_output_context2 failed: %s",
                   config_.stage_name.c_str(),
                   ffmpegErrorToString(ret).c_str());
        return false;
    }

    video_stream_ = avformat_new_stream(fmt_ctx_, nullptr);///参数2 const AVCodec * 在创建轨道的同时，是否直接关联一个指定的 FFmpeg 软件编码器结构体。
    if (!video_stream_) {
        RKCAM_LOGE("[%s] avformat_new_stream failed",
                   config_.stage_name.c_str());
        closeMuxer();
        return false;
    }

    /*
     * 你的 EncodedPacket pts_us 是微秒。
     * 所以 stream time_base 用 1/1000000。
     */
    video_stream_ -> time_base = AVRational{1, 1000000};
    video_stream_ -> avg_frame_rate = AVRational{config_.fps, 1};
    video_stream_ -> r_frame_rate = AVRational{config_.fps, 1};

    AVCodecParameters* par = video_stream_->codecpar;
    par->codec_type = AVMEDIA_TYPE_VIDEO;
    par->codec_id = AV_CODEC_ID_H264;
    par->width = config_.width;
    par->height = config_.height;
    par->codec_tag = 0;
    par->format = AV_PIX_FMT_NONE;

    par->extradata = static_cast<uint8_t*>(av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if(!par->extradata)
    {
        RKCAM_LOGE("[%s] av_mallocz extradata failed",
                   config_.stage_name.c_str());
        closeMuxer();
        return false;
    }
    std::memcpy(par->extradata, extradata.data(), extradata.size());
    par->extradata_size = static_cast<int>(extradata.size());

    if(!(fmt_ctx_->oformat->flags & AVFMT_NOFILE))
    {
        ret = avio_open(&fmt_ctx_->pb, config_.output_path.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            RKCAM_LOGE("[%s] avio_open failed: %s path=%s",
                        config_.stage_name.c_str(),
                        ffmpegErrorToString(ret).c_str(),
                        config_.output_path.c_str());
            closeMuxer();
            return false;
        }
    }


    ret = avformat_write_header(fmt_ctx_, nullptr);
    if (ret < 0) {
        RKCAM_LOGE("[%s] avformat_write_header failed: %s",
                   config_.stage_name.c_str(),
                   ffmpegErrorToString(ret).c_str());
        closeMuxer();
        return false;
    }
    first_pts_us_ = packet.pts_us >= 0 ? packet.pts_us : 0;
    header_written_ = true;


    RKCAM_LOGI("[%s] mp4 muxer initialized: %s %dx%d fps=%d extradata=%zu",
               config_.stage_name.c_str(),
               config_.output_path.c_str(),
               config_.width,
               config_.height,
               config_.fps,
               extradata.size());

    return true;

}
bool Mp4RecordStage::writePacket(const EncodedPacket& packet)
{
    if (!fmt_ctx_ || !video_stream_ || !header_written_) {
        RKCAM_LOGE("[%s] writePacket failed: muxer is not initialized",
                   config_.stage_name.c_str());
        return false;
    }

    if (packet.empty()) {
        return true;
    }

    /*
    * 关键：
    *   这里不再 av_new_packet，不再 memcpy。
    *
    * packet.data 是 EncodedPacket 自己持有的 vector。
    * av_write_frame() 在本函数内同步写入。
    * 函数返回前 packet.data 一直有效。
    *
    * 当前是单视频流，没有音频交织，所以使用 av_write_frame()。
    */
    AVPacket avpkt;
    av_init_packet(&avpkt);

    avpkt.data = const_cast<uint8_t*>(packet.data());
    avpkt.size = static_cast<int>(packet.size());
    avpkt.stream_index = video_stream_->index;
    avpkt.pos = -1;


    const AVRational input_time_base = AVRational{1, 1000000};
    int64_t pts_us = packet.pts_us;
    int64_t dts_us = packet.dts_us;

    const int64_t rel_pts_us = pts_us - first_pts_us_;
    const int64_t rel_dts_us = dts_us - first_pts_us_;

    avpkt.pts = av_rescale_q(rel_pts_us, input_time_base, video_stream_->time_base);
    avpkt.dts = av_rescale_q(rel_dts_us, input_time_base, video_stream_->time_base);

    int64_t duration_us = packet.duration_us;
    if (duration_us <= 0) {
        duration_us = 1000000LL / config_.fps;
    }
    avpkt.duration = av_rescale_q(duration_us, input_time_base, video_stream_->time_base);

    if(packet.key_frame){
        avpkt.flags |= AV_PKT_FLAG_KEY;
    }
    /*
    * 这里传入的是 MPP 输出的 Annex-B H264 packet。
    * 因为 extradata 也是 Annex-B SPS/PPS，
    * FFmpeg mov/mp4 muxer 会内部处理 H264 NAL reformat。
    */
    const int ret = av_write_frame(fmt_ctx_, &avpkt);
    if (ret < 0) {
        RKCAM_LOGE("[%s] av_write_frame failed: %s",
                   config_.stage_name.c_str(),
                   ffmpegErrorToString(ret).c_str());
        return false;
    }

    return true;

}

void Mp4RecordStage::closeMuxer()
{
    if(!fmt_ctx_)
    {
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
    if(!(fmt_ctx_->oformat->flags & AVFMT_NOFILE) && fmt_ctx_->pb)
    {
        avio_closep(&fmt_ctx_->pb);
    }
    
    avformat_free_context(fmt_ctx_);

    fmt_ctx_ = nullptr;
    video_stream_ = nullptr;
    header_written_ = false;
    first_pts_us_ = -1;
}

}