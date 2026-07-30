#include "rkcam/pipeline/aac_encode_stage.hpp"

#include "rkcam/core/log.hpp"

#include <utility>

namespace rkcam {

AacEncodeStage::AacEncodeStage(
    const AacEncodeStageConfig& config,
    BlockingQueue<PipelineAudioFrame>& input_queue,
    BlockingQueue<EncodedPacket>& output_queue)
    : config_(config),
      input_queue_(input_queue),
      output_queue_(output_queue)
{
}

AacEncodeStage::~AacEncodeStage()
{
    stop();
}

bool AacEncodeStage::start()
{
    if(running_)
    {
        return true;
    }
    if(config_.stage_name.empty())
    {
        config_.stage_name = "aac_encode";
    }
    /*
     * 保持 stage stream_id 和 encoder stream_id 一致。
     */
    if(config_.encoder.stream_id.empty())
    {
        config_.encoder.stream_id = config_.stream_id;
    }

    encoder_ = std::make_unique<AacEncoder>(config_.encoder);
    if (!encoder_->init()) {
        RKCAM_LOGE("[%s] AacEncoder init failed",
                   config_.stage_name.c_str());
        encoder_.reset();
        return false;
    }
    input_frames_ = 0;
    encoded_packets_ = 0;
    failed_frames_ = 0;
    failed_pushes_ = 0;
    running_ = true;
    thread_ = std::thread(&AacEncodeStage::threadLoop, this);

    RKCAM_LOGI("[%s] AacEncodeStage started: stream=%s "
               "input=%dHz/%dch output=%dHz/%dch bitrate=%d frame_size=%d",
               config_.stage_name.c_str(),
               config_.stream_id.c_str(),
               config_.encoder.input_sample_rate,
               config_.encoder.input_channels,
               encoder_->outputSampleRate(),
               encoder_->outputChannels(),
               encoder_->bitRate(),
               encoder_->frameSize());

    return true;
}

void AacEncodeStage::stop()
{
    if(!running_ && !thread_.joinable())
    {
        return;
    }
    /*
     * 中间 stage stop 时：
     *   1. stop input_queue_，唤醒阻塞在 pop() 的线程；
     *   2. threadLoop 会 drain 队列内已有 AudioFrame；
     *   3. flush encoder；
     *   4. stop output_queue_，通知下游结束。
     *
     * 前提是你的 BlockingQueue::stop() 不清空已有数据。
     */
    input_queue_.stop();
    if(thread_.joinable())
    {
        thread_.join();
    }

    if(encoder_)
    {
        encoder_->close();
    }
    running_ = false;

    RKCAM_LOGI("[%s] AacEncodeStage stopped, input_frames=%d encoded_packets=%d failed_frames=%d failed_pushes=%d",
               config_.stage_name.c_str(),
               input_frames_,
               encoded_packets_,
               failed_frames_,
               failed_pushes_);

}

void AacEncodeStage::threadLoop()
{
    while(true)
    {
        // RKCAM_LOGI("AacEncodeStage::threadLoop: input_frames_: %d", input_frames_);
        PipelineAudioFrame frame;
        if(!input_queue_.pop(frame))
        {
            if(input_queue_.stopped())
            {
                break;
            }
            continue;
        }

        if(frame.empty())
        {
            ++failed_frames_;
            continue;
        }
        if(!encodeOneFrame(frame))
        {
            ++failed_frames_;
            if(output_queue_.stopped())
            {
                RKCAM_LOGE("[%s] output queue stopped, exit encode loop",
                           config_.stage_name.c_str());
                break;
            }
            if (config_.max_encode_failures > 0 &&
                failed_frames_ >= config_.max_encode_failures) {
                RKCAM_LOGE("[%s] too many encode failures=%d, exit",
                           config_.stage_name.c_str(),
                           failed_frames_);
                break;
            }
            continue;
        }

        ++input_frames_;
        if (config_.max_frames > 0 &&
            input_frames_ >= config_.max_frames) {
            RKCAM_LOGI("[%s] reached max_frames=%d",
                       config_.stage_name.c_str(),
                       config_.max_frames);
            break;
        }
    }

    
    flushEncoder();
    output_queue_.stop();
    running_ = false;
    RKCAM_LOGI("[%s] AacEncodeStage thread exit, input_frames=%d encoded_packets=%d failed_frames=%d failed_pushes=%d",
               config_.stage_name.c_str(),
               input_frames_,
               encoded_packets_,
               failed_frames_,
               failed_pushes_);

}


bool AacEncodeStage::encodeOneFrame(const PipelineAudioFrame& frame)
{
    if(!encoder_)
    {
        RKCAM_LOGE("[%s] encodeOneFrame failed: encoder is null",
                   config_.stage_name.c_str());
        return false;
    }

    std::vector<EncodedPacket> packets;

    if(!encoder_->encode(frame, packets))
    {
        RKCAM_LOGE("[%s] encoder.encode failed: frame_id=%lld pts=%lld samples=%d size=%zu",
                   config_.stage_name.c_str(),
                   static_cast<long long>(frame.frame_id),
                   static_cast<long long>(frame.pts_us),
                   frame.nb_samples,
                   frame.size());
        return false;

    }

    return pushPackets(packets);

}

bool AacEncodeStage::pushPackets(std::vector<EncodedPacket>& packets)
{
    for(auto& packet : packets)
    {
        normalizePacketMetadata(packet);
        const int64_t pts_us = packet.pts_us;
        const int64_t duration_us = packet.duration_us;
        const size_t size = packet.size();

        if(!output_queue_.push(std::move(packet)))
        {
            ++failed_pushes_;
            RKCAM_LOGE("[%s] output_queue push failed, failed_pushes=%d",
                       config_.stage_name.c_str(),
                       failed_pushes_);

            return false;
        }
        ++encoded_packets_;
        if (config_.log_interval > 0 &&
            encoded_packets_ % config_.log_interval == 0) {
            RKCAM_LOGI("[%s] encoded_packets=%d stream=%s pts=%lld duration=%lld size=%zu",
                       config_.stage_name.c_str(),
                       encoded_packets_,
                       config_.stream_id.c_str(),
                       static_cast<long long>(pts_us),
                       static_cast<long long>(duration_us),
                       size);
        }

    }
    return true;

}


bool AacEncodeStage::flushEncoder()
{
    if(!encoder_)
    {
        RKCAM_LOGE("[%s] encoder.flush failed",
                   config_.stage_name.c_str());
        return false;
    }
    std::vector<EncodedPacket> packets;
    if(!encoder_->flush(packets))
    {
        RKCAM_LOGE("[%s] encoder.flush failed",
                   config_.stage_name.c_str());
        return false;
    }
    if (!packets.empty()) {
        pushPackets(packets);
    }

    RKCAM_LOGI("[%s] encoder flushed, total encoded_packets=%d",
               config_.stage_name.c_str(),
               encoded_packets_);

    return true;

}

void AacEncodeStage::normalizePacketMetadata(EncodedPacket& packet)
{
    /*
     * AacEncoder 已经会填大部分字段。
     * Stage 这里再统一修正，保证下游只看到标准 raw AAC packet。
     */
    packet.stream_id = config_.stream_id;
    packet.media_type = MediaType::Audio;
    packet.codec = CodecType::AAC;

    packet.key_frame = false;
    packet.eos = false;

    if (packet.dts_us <= 0) {
        packet.dts_us = packet.pts_us;
    }

    /*
     * 注意：
     *   这里不填 sample_rate / channels / bit_rate / extradata。
     *
     * 你的当前设计是：
     *   音频 stream 信息由 Mp4RecordStageConfig / RtspPushStageConfig 初始化时提供；
     *   EncodedPacket 只表示 raw AAC 媒体数据。
     */
}

}//namespace rkcam