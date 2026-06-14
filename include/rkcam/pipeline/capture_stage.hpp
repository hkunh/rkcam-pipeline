#pragma once
#include "rkcam/pipeline/pipeline_stage.hpp"
#include "rkcam/pipeline/pipeline_video_frame.hpp"
#include "rkcam/video/v4l2_video_source.hpp"
#include "rkcam/core/blocking_queue.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace rkcam{

struct CaptureStageConfig{
    std::string stream_id = "cam0";
    V4L2VideoSourceConfig source;
    VideoMemoryType output_memory_type = VideoMemoryType::Cpu;
};

class CaptureStage : public IStage{

public:
    CaptureStage(
        const CaptureStageConfig& config,
        std::unique_ptr<IVideoSource> source,
        BlockingQueue<PipelineVideoFrame>& output_queue);
    ~CaptureStage() override;

    CaptureStage(const CaptureStage&) = delete;
    CaptureStage& operator=(const CaptureStage&) = delete;

    bool start() override;
    void stop() override;

private:
    void threadLoop();

    bool handleCpuFrame(const VideoSourceFrame& src);
    bool handleDmaFrame(const VideoSourceFrame& src);

private:
    CaptureStageConfig config_;

    std::unique_ptr<IVideoSource> source_;

    BlockingQueue<PipelineVideoFrame>& output_queue_;

    std::thread thread_;

    std::atomic<bool> running_{false};

};


}