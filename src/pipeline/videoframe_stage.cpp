#include "rkcam/pipeline/videoframe_tee_stage.hpp"

#include "rkcam/core/log.hpp"

namespace rkcam {

VideoFrameTeeStage::VideoFrameTeeStage(
    const VideoFrameTeeStageConfig& config,
    BlockingQueue<PipelineVideoFrame>& input_queue,
    const std::vector<BlockingQueue<PipelineVideoFrame>*>& output_queues)
    : config_(config),
      input_queue_(input_queue),
      output_queues_(output_queues)
{
}

VideoFrameTeeStage::~VideoFrameTeeStage()
{
    stop();
}

bool VideoFrameTeeStage::start()
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

    input_frames_ = 0;
    forwarded_frames_ = 0;
    failed_pushes_ = 0;

    running_ = true;
    thread_ = std::thread(&VideoFrameTeeStage::threadLoop, this);

    RKCAM_LOGI("[%s] VideoFrameTeeStage started, outputs=%zu",
               config_.stage_name.c_str(),
               output_queues_.size());

    return true;
}

void VideoFrameTeeStage::stop()
{
    if (!running_ && !thread_.joinable()) {
        return;
    }

    /*
     * 停止输入队列，唤醒阻塞在 input_queue_.pop() 的线程。
     *
     * 注意：
     *   如果 BlockingQueue::pop() 在 stopped 后仍会弹出残留数据，
     *   那 TeeStage 会把残留帧继续分发完再退出。
     */
    input_queue_.stop();

    if (thread_.joinable()) {
        thread_.join();
    }

    /*
     * TeeStage 是多个 output_queue 的生产者。
     * 停止时必须通知所有下游 EOF。
     */
    stopOutputQueues();

    running_ = false;

    RKCAM_LOGI("[%s] VideoFrameTeeStage stopped, input_frames=%d forwarded_frames=%d failed_pushes=%d",
               config_.stage_name.c_str(),
               input_frames_,
               forwarded_frames_,
               failed_pushes_);
}


void VideoFrameTeeStage::threadLoop()
{
    while(true)
    {
        PipelineVideoFrame frame;
        if(!input_queue_.pop(frame))
        {
            if(input_queue_.stopped())
            {
                break;
            }
            continue;
        }

        input_frames_++;

        bool all_outputs_ok = true;
        bool pushed_to_at_least_one = false;

        for(size_t i = 0; i < output_queues_.size(); i++)
        {
            auto* output_queue = output_queues_[i];
            if (!output_queue) {
                all_outputs_ok = false;
                ++failed_pushes_;
                continue;
            }

            /*
             * 这里是浅拷贝：
             *
             * PipelineVideoFrame 内部真正的大数据是：
             *   std::shared_ptr<VideoBuffer> buffer
             *
             * 所以这里不会复制图像数据，只会增加 shared_ptr 引用计数。
             */
            PipelineVideoFrame out = frame;
            if(!output_queue->push(std::move(out)))
            {
                all_outputs_ok = false;
                +failed_pushes_;
                RKCAM_LOGE("[%s] output_queue[%zu] push failed, stream=%s frame_id=%lld",
                           config_.stage_name.c_str(),
                           i,
                           frame.stream_id.c_str(),
                           static_cast<long long>(frame.frame_id));

                if (!config_.continue_on_output_fail) {
                    break;
                }

                continue;
            }
            pushed_to_at_least_one = true;
        }

        if (pushed_to_at_least_one) {
            ++forwarded_frames_;
        }

        if (config_.log_interval > 0 &&
            input_frames_ % config_.log_interval == 0) {
            RKCAM_LOGI("[%s] input_frames=%d forwarded_frames=%d failed_pushes=%d stream=%s frame_id=%lld all_ok=%d",
                       config_.stage_name.c_str(),
                       input_frames_,
                       forwarded_frames_,
                       failed_pushes_,
                       frame.stream_id.c_str(),
                       static_cast<long long>(frame.frame_id),
                       all_outputs_ok ? 1 : 0);
        }
    }
    /*
     * 正常 EOF 路径：
     *   上游停止 input_queue 后，TeeStage 退出。
     *   这里 stop 所有输出队列，通知下游退出。
     */
    stopOutputQueues();

    running_ = false;

    RKCAM_LOGI("[%s] VideoFrameTeeStage thread exit, input_frames=%d forwarded_frames=%d failed_pushes=%d",
               config_.stage_name.c_str(),
               input_frames_,
               forwarded_frames_,
               failed_pushes_);
}

void VideoFrameTeeStage::stopOutputQueues()
{
    for (auto* q : output_queues_) {
        if (q) {
            q->stop();
        }
    }
}

}