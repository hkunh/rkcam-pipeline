#include "rkcam/pipeline/videoframe_tee_stage.hpp"

#include "rkcam/core/log.hpp"

namespace rkcam {

VideoFrameTeeStage::VideoFrameTeeStage(
    const VideoFrameTeeStageConfig& config,
    BlockingQueue<PipelineVideoFrame>& input_queue,
    const std::vector<VideoFrameTeeOutputPort>& outputs)
    : config_(config),
      input_queue_(input_queue),
      outputs_(outputs)
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

    if (outputs_.empty()) {
        RKCAM_LOGE("[%s] start failed: no output queues",
                   config_.stage_name.c_str());
        return false;
    }

    for (size_t i = 0; i < outputs_.size(); ++i) {
        if (!outputs_[i].queue) {
            RKCAM_LOGE(
                "[%s] output[%zu] queue is null",
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
               outputs_.size());

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
bool VideoFrameTeeStage::setOutputEnabled(const std::string& name, bool enabled)
{
    std::lock_guard<std::mutex> lock(outputs_mutex_);

    for(auto& output : outputs_)
    {
        if(output.name != name)
        {
            continue;
        }
        if(output.enabled == enabled)
        {
            return true;
        }
        output.enabled = enabled;
        RKCAM_LOGI(
            "[%s] output gate changed: "
            "name=%s enabled=%d",
            config_.stage_name.c_str(),
            name.c_str(),
            enabled ? 1 : 0);

        return true;
    }
    RKCAM_LOGE(
        "[%s] output gate not found: %s",
        config_.stage_name.c_str(),
        name.c_str());

    return false;
}
bool VideoFrameTeeStage::outputEnabled(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(outputs_mutex_);
    for(const auto& output : outputs_)
    {
        if(output.name == name)
        {
            return output.enabled;
        }
    }
    return false;
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

        /*
         * 这里只快速snapshot当前启用的输出。
         *
         * 真正push时不持有outputs_mutex_。
         * 防止push时阻塞，导致outputs_mutex_别的线程拿不到
         * 当前允许微小竞态
         * 线程刚snapshot出 encode=ON
         * CameraPipeline关闭Gate
         * 刚snapshot的这一帧仍可能push进去
         */
        std::vector<std::pair<std::string, BlockingQueue<PipelineVideoFrame>*>> enabled_outputs;

        {
            std::lock_guard<std::mutex> lock(outputs_mutex_);
            for(const auto& output : outputs_)
            {
                if(!output.enabled)
                {
                    ++skipped_outputs_;
                    continue;
                }
                if(!output.queue)
                {
                    continue;
                }
                enabled_outputs.emplace_back(output.name, output.queue);
            }
        }

        bool all_outputs_ok = true;
        bool pushed_to_at_least_one = false;

        for(const auto& output : enabled_outputs)
        {
            const std::string& output_name = output.first;
            auto* output_queue = output.second;

            PipelineVideoFrame out = frame;
            if(!output_queue->push(std::move(out)))
            {
                all_outputs_ok = false;
                ++failed_pushes_;
                RKCAM_LOGE(
                    "[%s] output push failed: "
                    "output=%s stream=%s "
                    "frame_id=%lld",
                    config_.stage_name.c_str(),
                    output_name.c_str(),
                    frame.stream_id.c_str(),
                    static_cast<long long>(
                        frame.frame_id));
                if (!config_
                        .continue_on_output_fail) {
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
            input_frames_ %
                    config_.log_interval ==
                0) {

            RKCAM_LOGI(
                "[%s] input=%llu "
                "forwarded=%llu "
                "gate_skips=%llu "
                "failed=%llu "
                "stream=%s frame=%lld "
                "all_ok=%d",
                config_.stage_name.c_str(),
                static_cast<unsigned long long>(
                    input_frames_),
                static_cast<unsigned long long>(
                    forwarded_frames_),
                static_cast<unsigned long long>(
                    skipped_outputs_),
                static_cast<unsigned long long>(
                    failed_pushes_),
                frame.stream_id.c_str(),
                static_cast<long long>(
                    frame.frame_id),
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
    std::lock_guard<std::mutex> lock(outputs_mutex_);
    for(auto& output : outputs_)
    {
        if(output.queue)
        {
            output.queue->stop();
        }
    }
}

}