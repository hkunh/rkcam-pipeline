#include "rkcam/pipeline/fps_stage.hpp"
#include "rkcam/core/log.hpp"

namespace rkcam{

FpsStage::FpsStage(
    const FpsStageConfig& config,
    BlockingQueue<PipelineVideoFrame>& input_queue
): config_(config), input_queue_(input_queue)
{

}


FpsStage::~FpsStage()
{
    stop();
}
bool FpsStage::start()
{
    if(running_)
    {
        return true;
    }
    running_ = true;

    thread_ = std::thread(&FpsStage::threadLoop, this);

    RKCAM_LOGI("[%s] FpsStage started", config_.stage_name.c_str());
    return true;
}
void FpsStage::stop()
{
    if(!running_)
    {
        return;
    }

    /*
     * 这里调用 input_queue_.stop() 是为了唤醒阻塞在 pop() 的线程。
     *
     * 当前 FpsStage 是这个 input_queue 的唯一消费者，所以这样可以。
     * 后面如果一个 queue 被多个 stage 共享，建议由 CameraPipeline 统一 stop queue。
     */
    input_queue_.stop();

    running_ = false;
    if(thread_.joinable())
    {
        thread_.join();
    }
}
void FpsStage::threadLoop()
{
    while (running_) {
        PipelineVideoFrame frame;

        if (!input_queue_.pop(frame)) {
            /*
             * queue stop 后 pop 返回 false。
             * 如果 running_ 已经是 false，就退出线程。
             */
            if (!running_) {
                break;
            }

            continue;
        }

        updateStats(frame);
    }
}

void FpsStage::updateStats(const PipelineVideoFrame& frame)
{
    const std::string& stream_id = frame.stream_id;

    auto now = std::chrono::steady_clock::now();

    auto &stats = stream_stats_[stream_id];

    if(!stats.initialized)
    {
        stats.initialized = true;
        stats.frame_count = 0;
        stats.last_frame_id = frame.frame_id;
        stats.start_time = now;
        stats.last_print_time = now;
    }
    ++stats.frame_count;
    stats.last_frame_id = frame.frame_id;

    double elapsed_print = std::chrono::duration<double>(now - stats.last_print_time).count();
    if(elapsed_print < config_.print_interval_sec){
        return;
    }
    double elapsed_total =
        std::chrono::duration<double>(now - stats.start_time).count();

    double avg_fps = elapsed_total > 0.0 ? static_cast<double>(stats.frame_count) / elapsed_total : 0.0;

    RKCAM_LOGI("[%s] stream=%s frames_count=%lld last_frame_id=%lld avg_fps=%.2f queue_size=%zu dropped=%zu",
            config_.stage_name.c_str(),
            stream_id.c_str(),
            static_cast<long long>(stats.frame_count),
            static_cast<long long>(stats.last_frame_id),
            avg_fps,
            input_queue_.size(),
            input_queue_.droppedCount());

    stats.last_print_time = now;

}
}