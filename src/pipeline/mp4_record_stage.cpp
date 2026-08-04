#include "rkcam/pipeline/mp4_record_stage.hpp"
#include "rkcam/core/log.hpp"


extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
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
 * 另外，对于mp4， ffmpeg原本要求extradata是AVCC格式的，但是传入annexB,ffmpeg也会自动转换
 * FFmpeg 的 H.264 封装逻辑非常聪明。它发现你喂给它的是 Annex B 格式的 SPS/PPS，而 MP4 物理文件写入必须使用 AVCC 格式。
  *于是，它的底层源码会调用转换函数，自动帮你把这个 Annex B 的 extradata 重构、组装为标准 AVCC 格式的 AVCDecoderConfigurationRecord

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

static bool hasIdrNal(const EncodedPacket& packet)
{
    if (!packet.data() || packet.size() == 0) {
        return false;
    }
    std::vector<NalUnit> nals;
    if(!splitAnnexBNals(packet.data(), packet.size(), nals))
    {
        return false;
    }
    for(const auto& nal : nals)
    {
        if (!nal.data || nal.size == 0) {
            continue;
        }
        const uint8_t nal_header = nal.data[0];
        /*
         * forbidden_zero_bit 必须为 0。
         * 为 1 说明码流头异常。
         */
        if((nal_header & 0x80) != 0)
        {
            continue;
        }

        const uint8_t nal_type = nal_header & 0x1f;
        /*
         * H.264:
         * type 5 = coded slice of an IDR picture
         */
        if(nal_type == 5)
        {
            return true;
        }
    }
    return false;
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
static bool buildAacLcAudioSpecificConfig(int sample_rate, int channels, std::vector<uint8_t>& extradata)
{
    extradata.clear();
    const int sample_index = aacSampleRateIndex(sample_rate);
    if(sample_index < 0)
    {
        return false;
    } 
    if(channels <= 0 || channels > 7)
    {
        return false;
    }

    /*
     * AAC-LC:
     *   audioObjectType = 2
     *
     * AudioSpecificConfig bits:
     *   audioObjectType       5 bits
     *   samplingFrequencyIdx  4 bits
     *   channelConfiguration  4 bits
     */
    const int audio_object_type = 2;
    const uint16_t asc = static_cast<uint16_t>((audio_object_type << 11) |
                        (sample_index << 7) |
                        (channels << 3));
    extradata.push_back(static_cast<uint8_t>((asc >> 8) & 0xff));
    extradata.push_back(static_cast<uint8_t>(asc & 0xff));
    return true;
}

static bool setCodecparExtradata(AVCodecParameters* par, const std::vector<uint8_t>& extradata)
{
    if(!par || extradata.empty())
    {
        return false;
    }

    par->extradata = static_cast<uint8_t*>(av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if(!par->extradata)
    {
        return false;
    }

    std::memcpy(par->extradata, extradata.data(), extradata.size());
    par->extradata_size = static_cast<int>(extradata.size());
    return true;
}
}//namespace

Mp4RecordStage::Mp4RecordStage(
    const Mp4RecordStageConfig& config,
    const std::vector<EncodedPacketInputPort>& input_ports)
    : config_(config),
      input_ports_(input_ports)
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

    if(!normalizeAndValidateConfig())
    {
        return false;
    }
    if (!validateInputPorts()) {
        return false;
    }

    saved_packets_ = 0;
    saved_video_packets_ = 0;
    saved_audio_packets_ = 0;
    failed_packets_ = 0;
    dropped_audio_before_header_ = 0;


    first_pts_us_ = -1;
    header_written_ = false;

    pending_audio_packets_.clear();

    input_eos_.assign(input_ports_.size(), false);

    internal_queue_.reset();

    running_ = true;
    /*
     * 先启动消费者，再启动生产者。
     */
    mux_thread_ = std::thread(&Mp4RecordStage::muxWriterLoop, this);

    input_threads_.clear();
    input_threads_.reserve(input_ports_.size());
    for(size_t i = 0; i < input_ports_.size(); ++i)
    {
        //注意，这里不是push_back，而是emplace_back
        // 这个emplace_back是编译器直接拿着 (&Mp4RecordStage::inputReaderLoop, this, i) 这三个参数，
        // 在 input_threads_ 的内存空间里直接执行 new std::thread(...)！
        input_threads_.emplace_back(&Mp4RecordStage::inputReaderLoop, this, i);
    }


    RKCAM_LOGI("[%s] Mp4RecordStage started: output=%s inputs=%zu video=%d audio=%d",
               config_.stage_name.c_str(),
               config_.output_path.c_str(),
               input_ports_.size(),
               config_.video.enabled ? 1 : 0,
               config_.audio.enabled ? 1 : 0);

    return true;
}

bool Mp4RecordStage::normalizeAndValidateConfig()
{
    if(config_.stage_name.empty())
    {
        config_.stage_name = "mp4_record";
    }
    if(config_.output_path.empty())
    {
        RKCAM_LOGE("[%s] output_path is empty",
                   config_.stage_name.c_str());
        return false;
    }
    /*
     * 兼容旧配置：
     *   如果新 video 配置没填，就用旧字段。
     */
    // if (config_.video.width <= 0 && config_.width > 0) {
    //     config_.video.width = config_.width;
    // }
    // if (config_.video.height <= 0 && config_.height > 0) {
    //     config_.video.height = config_.height;
    // }
    // if (config_.video.fps <= 0 && config_.fps > 0) {
    //     config_.video.fps = config_.fps;
    // }
    // if (config_.video.codec == CodecType::Unknown &&
    //     config_.codec != CodecType::Unknown) {
    //     config_.video.codec = config_.codec;
    // }

    if (!config_.video.enabled && !config_.audio.enabled) {
        RKCAM_LOGE("[%s] both video and audio are disabled",
                   config_.stage_name.c_str());
        return false;
    }

    if (config_.video.enabled) {
        if (config_.video.codec != CodecType::H264) {
            RKCAM_LOGE("[%s] only H264 video is supported now, codec=%d",
                       config_.stage_name.c_str(),
                       static_cast<int>(config_.video.codec));
            return false;
        }

        if (config_.video.width <= 0 ||
            config_.video.height <= 0 ||
            config_.video.fps <= 0) {
            RKCAM_LOGE("[%s] invalid video config: %dx%d fps=%d",
                       config_.stage_name.c_str(),
                       config_.video.width,
                       config_.video.height,
                       config_.video.fps);
            return false;
        }
    }

    if (config_.audio.enabled) {
        if (config_.audio.codec != CodecType::AAC) {
            RKCAM_LOGE("[%s] only AAC audio is supported now, codec=%d",
                       config_.stage_name.c_str(),
                       static_cast<int>(config_.audio.codec));
            return false;
        }

        if (config_.audio.sample_rate <= 0 ||
            config_.audio.channels <= 0 ||
            config_.audio.channels > 7 ||
            config_.audio.bit_rate <= 0) {
            RKCAM_LOGE("[%s] invalid audio config: rate=%d channels=%d bitrate=%d",
                       config_.stage_name.c_str(),
                       config_.audio.sample_rate,
                       config_.audio.channels,
                       config_.audio.bit_rate);
            return false;
        }

        if (config_.audio.extradata.empty()) {
            if (!buildAacLcAudioSpecificConfig(
                    config_.audio.sample_rate,
                    config_.audio.channels,
                    config_.audio.extradata)) {
                RKCAM_LOGE("[%s] build AAC-LC extradata failed",
                           config_.stage_name.c_str());
                return false;
            }
        }
    }

    return true;
}

bool Mp4RecordStage::validateInputPorts() const
{
    if (input_ports_.empty()) {
        RKCAM_LOGE("[%s] no input ports",
                   config_.stage_name.c_str());
        return false;
    }

    int video_count = 0;
    int audio_count = 0;

    for (size_t i = 0; i < input_ports_.size(); ++i) {
        const auto& port = input_ports_[i];

        if (!port.queue) {
            RKCAM_LOGE("[%s] input port[%zu] queue is null",
                       config_.stage_name.c_str(),
                       i);
            return false;
        }

        if (port.media_type == MediaType::Video) {
            ++video_count;
        } else if (port.media_type == MediaType::Audio) {
            ++audio_count;
        } else {
            RKCAM_LOGE("[%s] input port[%zu] has invalid media type",
                       config_.stage_name.c_str(),
                       i);
            return false;
        }
    }

    if (config_.video.enabled && video_count != 1) {
        RKCAM_LOGE("[%s] enabled video requires exactly one video port, got=%d",
                   config_.stage_name.c_str(),
                   video_count);
        return false;
    }

    if (config_.audio.enabled && audio_count != 1) {
        RKCAM_LOGE("[%s] enabled audio requires exactly one audio port, got=%d",
                   config_.stage_name.c_str(),
                   audio_count);
        return false;
    }

    return true;
}

void Mp4RecordStage::inputReaderLoop(size_t port_index)
{
    const auto& port = input_ports_[port_index];
    RKCAM_LOGI("[%s] input reader started: port=%s index=%zu",
               config_.stage_name.c_str(),
               port.port_name.c_str(),
               port_index);
    while(true)
    {
        EncodedPacket packet;
        if(!port.queue->pop(packet))
        {
            if(port.queue->stopped())
            {
                break;
            }
            continue;
        }
        if(packet.eos)
        {
            break;
        }
        InputEvent event;
        event.type = InputEventType::Packet;
        event.port_index = port_index;
        event.packet = std::move(packet);
        if(!internal_queue_.push(std::move(event)))
        {
            break;
        }
    }
    /*
     * 不直接 stop internal queue。
     * 这里只报告当前输入结束。
     */
    InputEvent eos;
    eos.type = InputEventType::Eos;
    eos.port_index = port_index;
    internal_queue_.push(std::move(eos));
    RKCAM_LOGI("[%s] input reader exit: port=%s index=%zu",
               config_.stage_name.c_str(),
               port.port_name.c_str(),
               port_index);
}
void Mp4RecordStage::stop()
{
    if (!running_ &&
        !mux_thread_.joinable() &&
        input_threads_.empty()) {
        return;
    }

    /*
     * 这些输入队列应当是该 sink 的专用分支队列。
     *
     * stop 不清空已有数据：
     * reader 会先 drain，再发送 EOS。
     */
    for (auto& port : input_ports_) {
        if (port.queue) {
            port.queue->stop();
        }
    }

    /*
     * 等所有 reader 把外部队列数据搬入 internal queue，
     * 并发送各自 EOS。
     */
    for (auto& thread : input_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    input_threads_.clear();
    if (mux_thread_.joinable()) {
        mux_thread_.join();
    }

    running_ = false;
    closeMuxer();  //在mux_thread_中已经关闭了muxer，这里保险再关闭一次。函数内部允许重复关闭
    RKCAM_LOGI("[%s] Mp4RecordStage stopped: saved=%d video=%d audio=%d failed=%d",
               config_.stage_name.c_str(),
               saved_packets_,
               saved_video_packets_,
               saved_audio_packets_,
               failed_packets_);

}

void Mp4RecordStage::muxWriterLoop()
{
    size_t eos_count = 0;
    
    while(true)
    {
        InputEvent event;
        if(!internal_queue_.pop(event))  //阻塞式的
        {
            break;
        }
        if(event.type == InputEventType::Eos)
        {
            if(event.port_index < input_eos_.size() && !input_eos_[event.port_index])
            {
                input_eos_[event.port_index] = true;
                ++eos_count;
            }
            RKCAM_LOGI("[%s] input eos: port=%s eos=%zu/%zu",
                       config_.stage_name.c_str(),
                       input_ports_[event.port_index]
                           .port_name.c_str(),
                       eos_count,
                       input_ports_.size());

            /*
             * 所有 reader 都已把自己的 packet 推完，
             * 然后才会发送 EOS。
             *
             * 因此收到全部 EOS 时，前面的媒体 event
             * 已经全部处理完。
             */
            if(eos_count == input_ports_.size())
            {
                break;
            }
            continue;
        }
        if(!handleInputPacket(event.port_index, std::move(event.packet)))
        {
            ++failed_packets_;
        }

    }
    /*
     * 所有 FFmpeg 关闭动作也在同一个线程完成。
     */
    closeMuxer();  // 必须在唯一 mux 线程中执行
    running_ = false;
    RKCAM_LOGI("[%s] mux writer exit: saved=%d video=%d audio=%d failed=%d",
               config_.stage_name.c_str(),
               saved_packets_,
               saved_video_packets_,
               saved_audio_packets_,
               failed_packets_);

}
bool Mp4RecordStage::validatePacketForPort(
    const EncodedPacketInputPort& port,
    const EncodedPacket& packet) const
{
    //检查 packet 是否来自正确端口
    if (packet.media_type != port.media_type) {
        RKCAM_LOGE("[%s] packet media mismatch: port=%s expected=%d actual=%d",
                   config_.stage_name.c_str(),
                   port.port_name.c_str(),
                   static_cast<int>(port.media_type),
                   static_cast<int>(packet.media_type));
        return false;
    }

    if (port.codec != CodecType::Unknown &&
        packet.codec != port.codec) {
        RKCAM_LOGE("[%s] packet codec mismatch: port=%s expected=%d actual=%d",
                   config_.stage_name.c_str(),
                   port.port_name.c_str(),
                   static_cast<int>(port.codec),
                   static_cast<int>(packet.codec));
        return false;
    }

    if (!port.stream_id.empty() &&
        packet.stream_id != port.stream_id) {
        RKCAM_LOGE("[%s] packet stream mismatch: port=%s expected=%s actual=%s",
                   config_.stage_name.c_str(),
                   port.port_name.c_str(),
                   port.stream_id.c_str(),
                   packet.stream_id.c_str());
        return false;
    }

    return true;
}
bool Mp4RecordStage::handleInputPacket(size_t port_index, EncodedPacket&& packet)
{
    if (port_index >= input_ports_.size()) {
        return false;
    }

    const auto& port = input_ports_[port_index];

    if (!validatePacketForPort(port, packet)) {
        return false;
    }

    if(packet.empty())
    {
        return true;
    }
    if(!header_written_)
    {
        return handlePacketBeforeHeader(std::move(packet));
    }
    if(!writePacket(packet))
    {
        return false;
    }
    accountSavedPacket(packet);
    return true;
}
bool Mp4RecordStage::handlePacketBeforeHeader(EncodedPacket&& packet)
{
    /*
     * 视频配置完整时，可以由第一包任意媒体 packet
     * 确定起始时间并直接初始化。
     */
    if(!config_.video.enabled)
    {
        const int64_t start_pts_us = packet.pts_us >= 0 ? packet.pts_us : 0;
        if(!initMuxerWithVideoExtradata(config_.video.extradata, start_pts_us))  //在函数内部会验证是否有开启video
        {
            return false;
        }

        if(!writePacket(packet)) //在函数内部会验证是否有开启video
        {
            return false;
        }
        accountSavedPacket(packet);
        return true;
    }

    /*
     * H264 extradata 还未知。
     */
    if(packet.media_type == MediaType::Audio)
    {
        if(pending_audio_packets_.size() >= config_.max_pending_audio_packets)
        {
            pending_audio_packets_.pop_front();
            ++dropped_audio_before_header_;
        }
        pending_audio_packets_.push_back(std::move(packet));
        return true;
    }
    if (packet.media_type != MediaType::Video ||
        packet.codec != CodecType::H264) {
        return false;
    }

    /*
     * 必须从第一帧可独立解码的 IDR 开始。
     */
    if (!packet.key_frame &&!hasIdrNal(packet)) {
        return true;
    }

    std::vector<uint8_t> extradata = config_.video.extradata;
    if(extradata.empty())
    {
        return initializeFromFirstVideoPacket(std::move(packet));
    }
    else{
        const int64_t start_pts = std::max<int64_t>(0, packet.pts_us);
        if (!initMuxerWithVideoExtradata(
                extradata,
                start_pts)) {
            return false;
        }
        return flushAudioPackets(std::move(packet));
    }

}
bool Mp4RecordStage::initializeFromFirstVideoPacket(EncodedPacket&& video_packet)
{
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    std::vector<uint8_t> video_extradata;
    if(!extractSpsPpsFromAnnexB(video_packet.buffer->data, sps, pps))
    {
        /*
         * 当前视频包没有 SPS/PPS，继续等下一包。
         *
         * 注意：
         * 当前这个普通视频包会丢弃。
         */
        return true;
    }

    if(!buildAnnexBExtradata(sps, pps, video_extradata))
    {
        return false;  
    }


    /*
     * 第一帧有效视频就是录像时间轴起点。
     */
    const int64_t start_pts_us =
        video_packet.pts_us >= 0
            ? video_packet.pts_us
            : (video_packet.dts_us >= 0
                   ? video_packet.dts_us
                   : 0);

    /*
     * 这里会创建音视频 stream、写 MP4 header，
     * 并把 first_pts_us_ 设置为 start_pts_us。
     */
    if(!initMuxerWithVideoExtradata(video_extradata, start_pts_us))
    {
        return false;
    }

    return flushAudioPackets(std::move(video_packet));
}

bool Mp4RecordStage::flushAudioPackets(EncodedPacket&& first_video_packet)
{
    /*
     * 当前策略：
     *   第一帧有效视频就是 MP4 时间轴起点。
     */
    /*
     * 第一包写入的媒体数据必须是第一帧视频。
     */
    if (!writeVideoPacket(first_video_packet)) {
        RKCAM_LOGE(
            "[%s] write first video packet failed: pts=%lld size=%zu",
            config_.stage_name.c_str(),
            static_cast<long long>(first_video_packet.pts_us),
            first_video_packet.size());
        return false;
    }
    /*
     * 第一帧视频也要计入统计。
     */
    accountSavedPacket(first_video_packet);
    /*
     * pending_audio_packets_ 只包含同一个 AAC 流的包，
     * 它们本身已经保持音频流内部顺序，不需要排序。
     */
    while (!pending_audio_packets_.empty()) {
        EncodedPacket audio_packet =
            std::move(pending_audio_packets_.front());

        pending_audio_packets_.pop_front();

        const int64_t audio_pts_us =
            audio_packet.pts_us >= 0
                ? audio_packet.pts_us
                : audio_packet.dts_us;
        /*
         * 录像以第一帧视频为起点。
         * 起点之前的音频不写入。
         */
        if (audio_pts_us < first_pts_us_ + 10) {   //实际测试30fps下，音频快一帧，这里10us让音频慢一些
            ++dropped_audio_before_header_;

            continue;
        }
        /*
         * 这包音频虽然比视频事件更早进入内部队列，
         * 但它的时间戳位于录像起点以后，所以必须保留。
         */
        if (!writeAudioPacket(audio_packet)) {
            RKCAM_LOGE(
                "[%s] write pending audio failed: "
                "pts=%lld size=%zu",
                config_.stage_name.c_str(),
                static_cast<long long>(
                    audio_packet.pts_us),
                audio_packet.size());

            ++failed_packets_;
            return false;
        }
        accountSavedPacket(audio_packet);
    }
    return true;
}
void Mp4RecordStage::accountSavedPacket(
    const EncodedPacket& packet)
{
    //统计写入包
    ++saved_packets_;

    if (packet.media_type == MediaType::Video) {
        ++saved_video_packets_;
    } else if (packet.media_type == MediaType::Audio) {
        ++saved_audio_packets_;
    }

    if (config_.log_interval > 0 &&
        saved_packets_ % config_.log_interval == 0) {
        RKCAM_LOGI("[%s] saved=%d video=%d audio=%d pts=%lld size=%zu",
                   config_.stage_name.c_str(),
                   saved_packets_,
                   saved_video_packets_,
                   saved_audio_packets_,
                   static_cast<long long>(packet.pts_us),
                   packet.size());
    }
}
// void Mp4RecordStage::threadLoop()
// {
//     // header 前：
//     // audio packet 先丢弃
//     // video packet 触发 muxer 初始化

//     // header 后：
//     //     video/audio 都写入 MP4
//     while(true)
//     {
//         EncodedPacket packet;
//         if(!input_queue_.pop(packet))
//         {
//             if(input_queue_.stopped())
//             {
//                 break;
//             }
//             continue;
//         }


//         if (packet.eos) {
//             RKCAM_LOGI("[%s] got eos packet",
//                        config_.stage_name.c_str());
//             break;
//         }
//         if (packet.empty()) {
//             continue;
//         }

//         if(!header_written_)
//         {
//             if(packet.media_type == MediaType::Audio)
//             {
//                 ++dropped_audio_before_header_;
//                 if (dropped_audio_before_header_ <= 5) {
//                     RKCAM_LOGI("[%s] drop audio before mp4 header: pts=%lld size=%zu",
//                                config_.stage_name.c_str(),
//                                static_cast<long long>(packet.pts_us),
//                                packet.size());
//                 }

//                 continue;
//             }
//             if (packet.media_type != MediaType::Video ||
//                 packet.codec != CodecType::H264) {
//                 ++failed_packets_;
//                 RKCAM_LOGE("[%s] wait video header, got unsupported packet: media=%d codec=%d",
//                            config_.stage_name.c_str(),
//                            static_cast<int>(packet.media_type),
//                            static_cast<int>(packet.codec));
//                 continue;
//             }



//             if(!initMuxerFromVideoPacket(packet))
//             {
//                 RKCAM_LOGE("[%s] initMuxerFromPacket failed, wait next packet, pts=%lld size=%zu",
//                            config_.stage_name.c_str(),
//                            static_cast<long long>(packet.pts_us),
//                            packet.size());
//                 continue;
//             }
//         }

//         if(!writePacket(packet))
//         {
//             RKCAM_LOGE("[%s] writePacket failed, pts=%lld size=%zu",
//                        config_.stage_name.c_str(),
//                        static_cast<long long>(packet.pts_us),
//                        packet.size());
//             ++failed_packets_;
//             continue;
//         }

//         ++saved_packets_;
//         if (packet.media_type == MediaType::Video) {
//             ++saved_video_packets_;
//         } else if (packet.media_type == MediaType::Audio) {
//             ++saved_audio_packets_;
//         }
//         if (config_.log_interval > 0 &&
//             saved_packets_ % config_.log_interval == 0) {
//             RKCAM_LOGI("[%s] saved_packets=%d video=%d audio=%d pts=%lld size=%zu",
//                        config_.stage_name.c_str(),
//                        saved_packets_,
//                        saved_video_packets_,
//                        saved_audio_packets_,
//                        static_cast<long long>(packet.pts_us),
//                        packet.size());
//         }
//     }
    
//     running_ = false;
//     RKCAM_LOGI("[%s] Mp4RecordStage thread exit, saved_packets=%d failed_packets=%d",
//             config_.stage_name.c_str(),
//             saved_packets_,
//             failed_packets_);
// }



bool Mp4RecordStage::initMuxerWithVideoExtradata(const std::vector<uint8_t>& video_extradata, int64_t start_pts_us)
{
    if(header_written_)
    {
        return true;
    }
    int ret = avformat_alloc_output_context2
    (
        &fmt_ctx_,
        nullptr,
        "mp4",
        config_.output_path.c_str()
    );
    if(ret < 0 ||!fmt_ctx_)
    {
        RKCAM_LOGE("[%s] avformat_alloc_output_context2 failed: %s",
                   config_.stage_name.c_str(),
                   ffmpegErrorToString(ret).c_str());
        return false;
    }
    if(config_.video.enabled)
    {
        if(!addVideoStream(video_extradata))
        {
            closeMuxer();
            return false;
        }
    }

    if(config_.audio.enabled)
    {
        if(!addAudioStream())
        {
            closeMuxer();
            return false;
        }
    }

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
    if(ret < 0)
    {
        RKCAM_LOGE("[%s] avformat_write_header failed: %s",
                   config_.stage_name.c_str(),
                   ffmpegErrorToString(ret).c_str());
        closeMuxer();
        return false;
    }

    first_pts_us_ = start_pts_us;
    header_written_ = true;
    RKCAM_LOGI("[%s] mp4 muxer initialized: %s video=%dx%d@%d audio=%d/%dch video_extra=%zu audio_extra=%zu",
               config_.stage_name.c_str(),
               config_.output_path.c_str(),
               config_.video.width,
               config_.video.height,
               config_.video.fps,
               config_.audio.sample_rate,
               config_.audio.channels,
               config_.video.extradata.empty() ? video_extradata.size()
                                               : config_.video.extradata.size(),
               config_.audio.extradata.size());

    return true;
}


bool Mp4RecordStage::addVideoStream(const std::vector<uint8_t>& extradata)
{
    if(extradata.empty())
    {
        RKCAM_LOGE("[%s] video extradata is empty",
                   config_.stage_name.c_str());
        return false;
    }
    video_stream_ = avformat_new_stream(fmt_ctx_, nullptr);
    if(!video_stream_)
    {
        RKCAM_LOGE("[%s] avformat_new_stream video failed",
                   config_.stage_name.c_str());
        return false;
    }

    video_stream_->time_base = AVRational{1, 1000000};
    video_stream_->avg_frame_rate = AVRational{config_.video.fps, 1};
    video_stream_->r_frame_rate = AVRational{config_.video.fps, 1};
    
    AVCodecParameters* par = video_stream_->codecpar;
    par->codec_type = AVMEDIA_TYPE_VIDEO;
    par->codec_id = AV_CODEC_ID_H264;
    par->width = config_.video.width;
    par->height = config_.video.height;
    par->codec_tag = 0;
    par->format = AV_PIX_FMT_NONE;
    par->bit_rate = config_.video.bit_rate;
    if(!setCodecparExtradata(par, extradata))
    {
        RKCAM_LOGE("[%s] set video extradata failed",
                   config_.stage_name.c_str());
        return false;
    }
    return true;

}

bool Mp4RecordStage::addAudioStream()
{
    std::vector<uint8_t> extradata = config_.audio.extradata;
    if(extradata.empty())
    {
        if(!buildAacLcAudioSpecificConfig(
            config_.audio.sample_rate,
            config_.audio.channels,
            extradata
        )){
            RKCAM_LOGE("[%s] build AAC-LC extradata failed",
                        config_.stage_name.c_str());
            return false;
        }
    }
    audio_stream_ = avformat_new_stream(fmt_ctx_, nullptr);
    if(!audio_stream_)
    {
        RKCAM_LOGE("[%s] avformat_new_stream audio failed",
                config_.stage_name.c_str());
        return false;
    }
    /*
    * audio stream 用 sample_rate time_base 更自然。
    */
    audio_stream_->time_base = AVRational{1, config_.audio.sample_rate};

    AVCodecParameters* par = audio_stream_->codecpar;
    par->codec_type = AVMEDIA_TYPE_AUDIO;
    par->codec_id = AV_CODEC_ID_AAC;
    par->codec_tag = 0;

    par->sample_rate = config_.audio.sample_rate;
    par->channels = config_.audio.channels;
    par->channel_layout = channelLayoutForChannels(config_.audio.channels);
    par->bit_rate = config_.audio.bit_rate;
    par->format = AV_SAMPLE_FMT_FLTP;

    if(!setCodecparExtradata(par, extradata))
    {
        RKCAM_LOGE("[%s] set audio extradata failed",
                   config_.stage_name.c_str());
        return false;
    }
     return true;

}

bool Mp4RecordStage::writePacket(const EncodedPacket& packet)
{
    if (!fmt_ctx_  || !header_written_) {
        RKCAM_LOGE("[%s] writePacket failed: muxer is not initialized",
                   config_.stage_name.c_str());
        return false;
    }

    if (packet.empty()) {
        return true;
    }
    if (packet.media_type == MediaType::Video) {
        return writeVideoPacket(packet);
    }

    if (packet.media_type == MediaType::Audio) {
        return writeAudioPacket(packet);
    }
    RKCAM_LOGE("[%s] unsupported media_type=%d",
               config_.stage_name.c_str(),
               static_cast<int>(packet.media_type));
    return false;

}
bool Mp4RecordStage::writeVideoPacket(const EncodedPacket& packet)
{
    if(!config_.video.enabled)
    {
        return true;
    }
    if(!video_stream_)
    {
        RKCAM_LOGE("[%s] video stream is null",
                   config_.stage_name.c_str());
        return false;
    }

    int64_t duration_us = packet.duration_us;
    if(duration_us <= 0)
    {
        duration_us = 1000000LL / config_.video.fps;
    }

    return writePacketToStream(packet, video_stream_, duration_us, true);

}
bool Mp4RecordStage::writeAudioPacket(const EncodedPacket& packet)
{
    if (!config_.audio.enabled) {
        return true;
    }

    if (!audio_stream_) {
        RKCAM_LOGE("[%s] audio stream is null",
                   config_.stage_name.c_str());
        return false;
    }
    if(packet.codec != CodecType::AAC)
    {
        RKCAM_LOGE("[%s] unsupported audio codec=%d",
                   config_.stage_name.c_str(),
                   static_cast<int>(packet.codec));
        return false;
    }
    /*
     * AAC-LC 一包通常 1024 samples。
     * 但优先使用 AacEncoder 给的 duration_us。
     */
     int64_t duration_us = packet.duration_us;
     if(duration_us <= 0)
     {
        duration_us = 1024 * 1000000LL / config_.audio.sample_rate;
     }
     return writePacketToStream(packet, audio_stream_, duration_us, false);
}

bool Mp4RecordStage::writePacketToStream(const EncodedPacket& packet, AVStream* stream, int64_t default_duration_us, bool is_video)
{
    if (!fmt_ctx_ || !stream || !packet.data() || packet.size() == 0) {
        return false;
    }
    AVPacket avpkt;
    av_init_packet(&avpkt);
    avpkt.data = nullptr;
    avpkt.size = 0;
    int ret = av_new_packet(&avpkt, static_cast<int>(packet.size()));
    if(ret < 0)
    {
        RKCAM_LOGE("[%s] av_new_packet failed: %s",
                   config_.stage_name.c_str(),
                   ffmpegErrorToString(ret).c_str());
        return false;
    }

    std::memcpy(avpkt.data, packet.data(), packet.size());
    avpkt.stream_index = stream->index;
    avpkt.pos = -1;
    const AVRational input_time_base = AVRational{1, 1000000};

    int64_t pts_us = packet.pts_us;
    int64_t dts_us = packet.dts_us > 0 ? packet.dts_us : packet.pts_us;
    if(first_pts_us_ >= 0)
    {
        pts_us -= first_pts_us_;
        dts_us -= first_pts_us_;
    }

    /*
     * 第一版：
     *   header 前音频已经丢弃。
     *   如果仍然遇到负时间戳，直接夹到 0，避免 muxer 报错。
     */
    if (pts_us < 0) {
        pts_us = 0;
    }
    if (dts_us < 0) {
        dts_us = pts_us;
    }

    avpkt.pts = av_rescale_q(pts_us, input_time_base, stream->time_base);
    avpkt.dts = av_rescale_q(dts_us, input_time_base, stream->time_base);
    avpkt.duration = av_rescale_q(default_duration_us, input_time_base, stream->time_base);
    if(is_video && packet.key_frame)
    {
        avpkt.flags |= AV_PKT_FLAG_KEY;
    }
    /*
     * 音视频混合必须使用 interleaved 写法。
     * av_interleaved_write_frame() 可能暂存 packet，
     * 所以上面必须用 av_new_packet() 让 AVPacket 自己持有数据。
     */
    ret = av_interleaved_write_frame(fmt_ctx_, &avpkt);
    if (ret < 0) {
        av_packet_unref(&avpkt);

        RKCAM_LOGE("[%s] av_interleaved_write_frame failed: %s media=%d pts=%lld dts=%lld duration=%lld size=%zu",
                   config_.stage_name.c_str(),
                   ffmpegErrorToString(ret).c_str(),
                   static_cast<int>(packet.media_type),
                   static_cast<long long>(packet.pts_us),
                   static_cast<long long>(packet.dts_us),
                   static_cast<long long>(default_duration_us),
                   packet.size());
        return false;
    }
    /*
     * 成功时 av_interleaved_write_frame() 会消费 packet。
     * 不再 av_packet_unref(&avpkt)，避免重复释放。
     */
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
    audio_stream_ = nullptr;
    header_written_ = false;
    first_pts_us_ = -1;
}

}