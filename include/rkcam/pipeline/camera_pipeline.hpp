#pragma once

#include "rkcam/core/blocking_queue.hpp"
#include "rkcam/pipeline/capture_stage.hpp"
#include "rkcam/pipeline/fps_stage.hpp"
#include "rkcam/pipeline/raw_save_stage.hpp"
#include "rkcam/pipeline/pipeline_stage.hpp"
#include "rkcam/pipeline/pipeline_video_frame.hpp"
#include "rkcam/video/v4l2_video_source.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace rkcam {

enum class CameraDebugStage {
    None,
    Fps,
    RawSave,
};

struct CameraPipelineConfig {
    std::string stream_id = "cam0";

    V4L2VideoSourceConfig source;

    VideoMemoryType capture_output_memory_type = VideoMemoryType::Cpu;

    size_t raw_queue_capacity = 4;
    QueueFullPolicy raw_queue_policy = QueueFullPolicy::DropOldest;

    /*
     * 当前 Fps 和 RawSave 互斥。
     * 因为它们都会从 raw_queue_ pop frame。
     */
    CameraDebugStage debug_stage = CameraDebugStage::Fps;

    FpsStageConfig fps;
    RawSaveStageConfig raw_save;
};

class CameraPipeline {
public:
    explicit CameraPipeline(const CameraPipelineConfig& config);
    ~CameraPipeline();

    CameraPipeline(const CameraPipeline&) = delete;
    CameraPipeline& operator=(const CameraPipeline&) = delete;

    bool start();
    void stop();

    bool isRunning() const;

private:
    bool createStages();
    bool startStages();
    void destroyStages();

private:
    CameraPipelineConfig config_;

    BlockingQueue<PipelineVideoFrame> raw_frame_queue_;

    std::unique_ptr<CaptureStage> capture_stage_;

    /*
     * 当前 debug_stage_ 可以是：
     *   FpsStage
     *   RawSaveStage
     *
     * 后面如果做 Tee/Dispatcher，可以扩展多个下游。
     */
    std::unique_ptr<IStage> debug_stage_;

    std::atomic<bool> running_{false};
};

} // namespace rkcam