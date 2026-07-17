#include "rkcam/pipeline/encoded_packet_tee_stage.hpp"

#include "rkcam/core/log.hpp"

namespace rkcam {
EncodedPacketTeeStage::EncodedPacketTeeStage(
    const EncodedPacketTeeStageConfig& config,
    BlockingQueue<EncodedPacket>& input_queue,
    const std::vector<BlockingQueue<EncodedPacket>*>& output_queues)
    : config_(config),
      input_queue_(input_queue),
      output_queues_(output_queues)
{
}

EncodedPacketTeeStage::~EncodedPacketTeeStage()
{
    stop();
}



bool EncodedPacketTeeStage::start()
{
    if (running_) {
        return true;
    }

    if (output_queues_.empty()) {
        RKCAM_LOGE("[%s] start failed: no output queues",
                   config_.stage_name.c_str());
        return false;
    }

    for (size_t i = 0; i < output_queues_.size(); ++i) {
        if (!output_queues_[i]) {
            RKCAM_LOGE("[%s] start failed: output_queue[%zu] is null",
                       config_.stage_name.c_str(),
                       i);
            return false;
        }
    }


    input_packets_ = 0;
    forwarded_packets_ = 0;
    failed_pushes_ = 0;

    running_ = true;
    thread_ = std::thread(&EncodedPacketTeeStage::threadLoop, this);

    RKCAM_LOGI("[%s] EncodedPacketTeeStage started, outputs=%zu",
               config_.stage_name.c_str(),
               output_queues_.size());

    return true;
}

void EncodedPacketTeeStage::threadLoop()
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
        
        bool pushed_to_at_least_one = false;

        for(size_t i = 0; i < output_queues_.size(); i++)
        {
            auto* output_queue = output_queues_[i];

            if(!output_queue)
            {
                ++failed_pushes_;
                continue;
            }

            /*
             * 注意：
             *
             * 如果 EncodedPacket 内部是 shared_ptr<EncodedBuffer>，
             * 这里是浅拷贝，不复制码流数据。
             *
             * 如果 EncodedPacket 内部还是 std::vector<uint8_t>，
             * 这里会复制一份压缩码流。
             * 第一版一般也能接受，但后面录像+推流建议改成 shared_ptr buffer。
             */
            EncodedPacket out = packet;
            if(!output_queue->push(std::move(out)))
            {
                ++failed_pushes_;

                RKCAM_LOGE("[%s] output_queue[%zu] push failed, stream=%s pts=%lld size=%zu",
                           config_.stage_name.c_str(),
                           i,
                           packet.stream_id.c_str(),
                           static_cast<long long>(packet.pts_us),
                           packet.size());
                if (!config_.continue_on_output_fail) {
                    break;
                }

                continue;     
            }
            pushed_to_at_least_one = true;
            if (pushed_to_at_least_one) {
                ++forwarded_packets_;
            }

            if (config_.log_interval > 0 &&
                input_packets_ % config_.log_interval == 0) {
                RKCAM_LOGI("[%s] input_packets=%d forwarded_packets=%d failed_pushes=%d "
                        "stream=%s pts=%lld size=%zu key=%d",
                        config_.stage_name.c_str(),
                        input_packets_,
                        forwarded_packets_,
                        failed_pushes_,
                        packet.stream_id.c_str(),
                        static_cast<long long>(packet.pts_us),
                        packet.size(),
                        packet.key_frame ? 1 : 0);
            }
        }
        
    }

    stopOutputQueues();
    running_ = false;
    RKCAM_LOGI("[%s] EncodedPacketTeeStage thread exit, input_packets=%d forwarded_packets=%d failed_pushes=%d",
               config_.stage_name.c_str(),
               input_packets_,
               forwarded_packets_,
               failed_pushes_);
}

void EncodedPacketTeeStage::stop()
{
    if(!running_ && !thread_.joinable())
    {
        return;
    }

    /*
     * 停止输入队列，唤醒 pop。
     */
    input_queue_.stop();

    if (thread_.joinable()) {
        thread_.join();
    }
    /*
     * EncodedPacketTeeStage 是多个输出队列的生产者。
     * 停止时通知所有下游 EOF。
     */
    stopOutputQueues();

    running_ = false;

    RKCAM_LOGI("[%s] EncodedPacketTeeStage stopped, input_packets=%d forwarded_packets=%d failed_pushes=%d",
               config_.stage_name.c_str(),
               input_packets_,
               forwarded_packets_,
               failed_pushes_);

}



void EncodedPacketTeeStage::stopOutputQueues(){
    for(auto* q : output_queues_)
    {
        if(q)
        {
            q->stop();
        }
    }
}

}