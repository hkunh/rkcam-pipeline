#pragma once

#include "rkcam/core/blocking_queue.hpp"
#include "rkcam/pipeline/pipeline_stage.hpp"
#include "rkcam/pipeline/pipeline_video_frame.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>

namespace rkcam{

struct FpsStageConfig{
    std::string stage_name = "fps";

    double print_interval_sec = 1.0;
};

class FpsStage : public IStage{
public:
    FpsStage(const FpsStageConfig& config, BlockingQueue<PipelineVideoFrame>& input_queue);
    ~FpsStage() override;

    FpsStage(const FpsStage&) = delete;
    FpsStage& operator=(const FpsStage&) = delete;
    
    bool start() override;
    void stop() override;

private:
    struct StreamStats{
        bool initialized = false;
        int64_t frame_count = 0;
        int64_t last_frame_id = -1;

        std::chrono::steady_clock::time_point start_time;
        std::chrono::steady_clock::time_point last_print_time;
    };
private:
    void threadLoop();
    void updateStats(const PipelineVideoFrame& frame);

private:
    FpsStageConfig config_;
    BlockingQueue<PipelineVideoFrame>& input_queue_;
    
    std::thread thread_;
    std::atomic<bool> running_{false};

    std::unordered_map<std::string, StreamStats> stream_stats_;

};

}