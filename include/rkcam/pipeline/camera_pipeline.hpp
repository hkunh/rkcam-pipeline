#pragma once

#include "rkcam/core/blocking_queue.hpp"
#include "rkcam/pipeline/capture_stage.hpp"
#include "rkcam/pipeline/fps_stage.hpp"
#include "rkcam/pipeline/raw_save_stage.hpp"
#include "rkcam/pipeline/rga_stage.hpp"
#include "rkcam/pipeline/pipeline_stage.hpp"
#include "rkcam/video/pipeline_video_frame.hpp"
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

    /*
     * CaptureStage:
     *   当前建议输出 DmaBuffer。
     *   max_frames 也放在 capture_stage_config 里。
     */
    CaptureStageConfig capture_stage_config;

    /*
     * RgaStage:
     *   当前写死插在 CaptureStage 和 DebugStage 之间。
     */
    RgaStageConfig rga_stage_config;

    /*
     * Capture -> RGA 的队列。
     */
    size_t raw_queue_capacity = 4;
    QueueFullPolicy raw_queue_policy = QueueFullPolicy::DropOldest;

    /*
     * RGA -> RawSave/Fps 的队列。
     */
    size_t processed_queue_capacity = 4;
    QueueFullPolicy processed_queue_policy = QueueFullPolicy::DropOldest;

    /*
     * 当前 debug_stage 消费的是 RGA 输出后的 processed_frame_queue_。
     */
    CameraDebugStage debug_stage = CameraDebugStage::RawSave;

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

    /*
     * CaptureStage -> RgaStage
     */
    BlockingQueue<PipelineVideoFrame> raw_frame_queue_;

    /*
     * RgaStage -> RawSaveStage / FpsStage
     */
    BlockingQueue<PipelineVideoFrame> processed_frame_queue_;

    std::unique_ptr<CaptureStage> capture_stage_;
    std::unique_ptr<RgaStage> rga_stage_;

    /*
     * 当前 debug_stage_ 可以是：
     *   FpsStage
     *   RawSaveStage
     *
     * 注意：它消费 processed_frame_queue_。
     */
    std::unique_ptr<IStage> debug_stage_;

    std::atomic<bool> running_{false};
};

} // namespace rkcam