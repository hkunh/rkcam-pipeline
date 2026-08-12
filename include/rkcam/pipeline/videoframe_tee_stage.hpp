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

struct VideoFrameTeeOutputPort{
    std::string name;
    BlockingQueue<PipelineVideoFrame>* queue = nullptr;
    bool enabled = true;
};





class VideoFrameTeeStage : public IStage {
public:
    VideoFrameTeeStage(
        const VideoFrameTeeStageConfig& config,
        BlockingQueue<PipelineVideoFrame>& input_queue,
        const std::vector<VideoFrameTeeOutputPort>& outputs);

    ~VideoFrameTeeStage() override;

    VideoFrameTeeStage(const VideoFrameTeeStage&) = delete;
    VideoFrameTeeStage& operator=(const VideoFrameTeeStage&) = delete;

    bool start() override;
    void stop() override;

    /*
     * 可以在Stage启动前或运行中调用。
     *
     * false：
     *   不再向该输出push新frame。
     *
     * 注意：
     *   不stop queue。
     */
    bool setOutputEnabled(const std::string& name, bool enabled);
    bool outputEnabled(const std::string& name) const;


private:
    void threadLoop();

    void stopOutputQueues();

private:
    VideoFrameTeeStageConfig config_;

    BlockingQueue<PipelineVideoFrame>& input_queue_;
    std::vector<VideoFrameTeeOutputPort> outputs_;

    mutable std::mutex outputs_mutex_; //允许该成员变量在 const 成员函数（常成员函数）中被修改

    std::thread thread_;
    std::atomic<bool> running_{false};

    uint64_t input_frames_ = 0;
    uint64_t forwarded_frames_ = 0;
    uint64_t skipped_outputs_ = 0;
    uint64_t failed_pushes_ = 0;
};

} // namespace rkcam