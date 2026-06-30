#include "rkcam/pipeline/mpp_stage.hpp"
#include "rkcam/core/log.hpp"

#include <utility>

namespace rkcam {

MppStage::MppStage(
    const MppStageConfig& config,
    BlockingQueue<PipelineVideoFrame>& input_queue,
    BlockingQueue<EncodedPacket>& output_queue)
    : config_(config),
      input_queue_(input_queue),
      output_queue_(output_queue),
      encoder_(config.encoder)
{
}

MppStage::~MppStage()
{
    stop();
}

bool MppStage::start()
{
    if(running_)
    {
        return true;
    }
    if(!encoder_.init())
    {
        RKCAM_LOGE("[%s] MppEncoder init failed",
                   config_.stage_name.c_str());
        return false;
    }
    encoded_packets_ = 0;
    failed_frames_ = 0;
    thread_ = std::thread(&MppStage::threadLoop, this);

    running_ = true;
    RKCAM_LOGI("[%s] MppStage started",
               config_.stage_name.c_str());

    return true;
}


void MppStage::stop()
{
    if(!running_ && !thread_.joinable())
    {
        return;
    }
    /*
    * drain stop:
    *
    * input_queue_.stop() 不会丢弃队列中已有 frame。
    * BlockingQueue::pop() 会继续弹出残留 frame。
    * MppStage 会把 input_queue_ 中剩余帧编码完，再退出。
    */
    input_queue_.stop();
    if(thread_.joinable())
    {
        thread_.join();
    }

    output_queue_.stop();

    encoder_.close();

    running_ = false;

    RKCAM_LOGI("[%s] MppStage stopped, encoded_packets=%d failed_frames=%d",
               config_.stage_name.c_str(),
               encoded_packets_,
               failed_frames_);  
}

void MppStage::threadLoop(){
    while(true){
        PipelineVideoFrame frame;
        if(!input_queue_.pop(frame)){
            if(input_queue_.stopped())
            {
                break;
            }
            continue;
        }
        EncodedPacket packet;
        if(!encoder_.encode(frame, packet)){
            ++failed_frames_;
            RKCAM_LOGE("[%s] encode failed, stream=%s frame_id=%lld",
                       config_.stage_name.c_str(),
                       frame.stream_id.c_str(),
                       static_cast<long long>(frame.frame_id));
            continue;
        }
        /*
        * 有些编码器调用可能成功但没有输出数据。
        * 你的当前 encode() 是阻塞等待 packet 的版本，
        * 正常情况下这里不会为空，但保留这个判断更稳。
        */
        if (packet.empty()) {
            RKCAM_LOGE("[%s] encode output empty packet, stream=%s frame_id=%lld",
                       config_.stage_name.c_str(),
                       frame.stream_id.c_str(),
                       static_cast<long long>(frame.frame_id));
            continue;
        }
        ++encoded_packets_;
        if(!output_queue_.push(std::move(packet)))
        {
            RKCAM_LOGE("[%s] output_queue push failed",
                       config_.stage_name.c_str());

                        /*
            * 下游已经停止，当前 stage 也停止继续消费上游。
            * 清掉 input_queue 中残留 frame，避免上游 DMA buffer 卡住。
            */
            if (output_queue_.stopped()) {
                input_queue_.stop();
                input_queue_.clear();
                break;
            }
            continue;
        }

    }
    output_queue_.stop();
    running_ = false;

    RKCAM_LOGI("[%s] MppStage thread exit, encoded_packets=%d failed_frames=%d",
               config_.stage_name.c_str(),
               encoded_packets_,
               failed_frames_);
}

}