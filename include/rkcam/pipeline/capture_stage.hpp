#pragma once
#include "rkcam/pipeline/pipeline_stage.hpp"
#include "rkcam/video/pipeline_video_frame.hpp"
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
    /*
     * <= 0 表示无限采集。
     * > 0 表示最多输出 max_frames 帧，然后停止 output_queue。
     */
    int max_frames = 0;
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

    std::mutex dma_mutex_;
    std::condition_variable dma_cv_;
    size_t dma_frames_in_flight_ = 0;

    int captured_frames_ = 0;
};


}