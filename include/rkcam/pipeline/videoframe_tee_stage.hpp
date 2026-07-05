#pragma once

#include "rkcam/core/blocking_queue.hpp"
#include "rkcam/pipeline/pipeline_stage.hpp"
#include "rkcam/video/video_frame.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace rkcam {

struct VideoFrameTeeStageConfig {
    std::string stage_name = "video_frame_tee";

    /*
     * 某个输出队列 push 失败时，是否继续推给其他输出队列。
     *
     * 预览 + 录像场景建议 true：
     *   预览分支停止/满了，不应该影响录像分支；
     *   录像分支停止/满了，也不应该影响预览分支。
     */
    bool continue_on_output_fail = true;

    /*
     * 每多少帧打印一次日志。
     * <= 0 表示不打印周期日志。
     */
    int log_interval = 30;
};

class VideoFrameTeeStage : public IStage {
public:
    VideoFrameTeeStage(
        const VideoFrameTeeStageConfig& config,
        BlockingQueue<PipelineVideoFrame>& input_queue,
        const std::vector<BlockingQueue<PipelineVideoFrame>*>& output_queues);

    ~VideoFrameTeeStage() override;

    VideoFrameTeeStage(const VideoFrameTeeStage&) = delete;
    VideoFrameTeeStage& operator=(const VideoFrameTeeStage&) = delete;

    bool start() override;
    void stop() override;

private:
    void threadLoop();

    void stopOutputQueues();

private:
    VideoFrameTeeStageConfig config_;

    BlockingQueue<PipelineVideoFrame>& input_queue_;
    std::vector<BlockingQueue<PipelineVideoFrame>*> output_queues_;

    std::thread thread_;
    std::atomic<bool> running_{false};

    int input_frames_ = 0;
    int forwarded_frames_ = 0;
    int failed_pushes_ = 0;
};

} // namespace rkcam