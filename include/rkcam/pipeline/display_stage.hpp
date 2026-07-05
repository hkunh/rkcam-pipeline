#pragma once

#include "rkcam/core/blocking_queue.hpp"
#include "rkcam/display/display_sink.hpp"
#include "rkcam/pipeline/pipeline_stage.hpp"
#include "rkcam/video/video_frame.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace rkcam {

struct DisplayStageConfig{

    std::string stage_name = "display";

    /*
     * 预览链路不要无限刷错误。
     * 连续失败超过该数量后退出显示线程。
     */
    int max_failed_frames = 30;
};

class DisplayStage : public IStage{
public:
    DisplayStage(const DisplayStageConfig& config, BlockingQueue<PipelineVideoFrame>& input_queue, std::unique_ptr<IDisplaySink> sink);
    ~DisplayStage();

    DisplayStage(const DisplayStage&) = delete;
    DisplayStage& operator=(const DisplayStage&) = delete;

    bool start() override;
    void stop() override;

private:
    void threadLoop();

private:
    DisplayStageConfig config_;

    BlockingQueue<PipelineVideoFrame>& input_queue_;
    std::unique_ptr<IDisplaySink> sink_;

    std::thread thread_;
    std::atomic<bool> running_{false};

    int displayed_frames_ = 0;
    int failed_frames_ = 0;

};



}