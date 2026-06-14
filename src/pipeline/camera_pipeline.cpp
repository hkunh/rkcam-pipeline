#include "rkcam/pipeline/camera_pipeline.hpp"
#include "rkcam/core/log.hpp"

#include <memory>
#include <utility>

namespace rkcam {

CameraPipeline::CameraPipeline(const CameraPipelineConfig& config)
    : config_(config),
      raw_frame_queue_(config.raw_queue_capacity, config.raw_queue_policy)
{
}

CameraPipeline::~CameraPipeline()
{
    stop();
}

bool CameraPipeline::start()
{
    if (running_) {
        return true;
    }

    /*
     * 支持 stop 后再次 start。
     */
    raw_frame_queue_.reset();

    if (!createStages()) {
        RKCAM_LOGE("[%s] CameraPipeline createStages failed",
                   config_.stream_id.c_str());
        destroyStages();
        return false;
    }

    if (!startStages()) {
        RKCAM_LOGE("[%s] CameraPipeline startStages failed",
                   config_.stream_id.c_str());
        stop();
        return false;
    }

    running_ = true;

    RKCAM_LOGI("[%s] CameraPipeline started",
               config_.stream_id.c_str());

    return true;
}

void CameraPipeline::stop()
{
    if (!running_ && !capture_stage_ && !debug_stage_) {
        return;
    }

    running_ = false;

    /*
     * 停止顺序：
     * 1. 先停上游 CaptureStage，避免继续 push。
     * 2. 再 stop queue，唤醒下游 pop。
     * 3. 再停下游 debug stage。
     */
    if (capture_stage_) {
        capture_stage_->stop();
        capture_stage_.reset();
    }

    raw_frame_queue_.stop();

    if (debug_stage_) {
        debug_stage_->stop();
        debug_stage_.reset();
    }

    RKCAM_LOGI("[%s] CameraPipeline stopped",
               config_.stream_id.c_str());
}

bool CameraPipeline::isRunning() const
{
    /*
     * RawSaveStage 保存够 max_frames 后可能会 stop queue。
     * 这里把 queue stopped 也作为 pipeline 结束条件之一。
     */
    return running_ && !raw_frame_queue_.stopped();
}

bool CameraPipeline::createStages()
{
    auto source = std::make_unique<V4L2VideoSource>(config_.source);

    CaptureStageConfig capture_config;
    capture_config.stream_id = config_.stream_id;
    capture_config.output_memory_type = config_.capture_output_memory_type;

    capture_stage_ = std::make_unique<CaptureStage>(
        capture_config,
        std::move(source),
        raw_frame_queue_);

    switch (config_.debug_stage) {
    case CameraDebugStage::None:
        debug_stage_.reset();
        break;

    case CameraDebugStage::Fps: {
        FpsStageConfig fps_config = config_.fps;
        if (fps_config.stage_name.empty()) {
            fps_config.stage_name = "fps";
        }

        debug_stage_ = std::make_unique<FpsStage>(
            fps_config,
            raw_frame_queue_);
        break;
    }

    case CameraDebugStage::RawSave: {
        RawSaveStageConfig save_config = config_.raw_save;
        if (save_config.stage_name.empty()) {
            save_config.stage_name = "raw_save";
        }

        debug_stage_ = std::make_unique<RawSaveStage>(
            save_config,
            raw_frame_queue_);
        break;
    }

    default:
        RKCAM_LOGE("[%s] unsupported debug stage",
                   config_.stream_id.c_str());
        return false;
    }

    return true;
}

bool CameraPipeline::startStages()
{
    /*
     * 先启动下游，再启动上游。
     * 这样 CaptureStage 一开始 push，下游已经能 pop。
     */
    if (debug_stage_) {
        if (!debug_stage_->start()) {
            RKCAM_LOGE("[%s] debug stage start failed",
                       config_.stream_id.c_str());
            return false;
        }
    }

    if (!capture_stage_) {
        RKCAM_LOGE("[%s] capture_stage_ is null",
                   config_.stream_id.c_str());
        return false;
    }

    if (!capture_stage_->start()) {
        RKCAM_LOGE("[%s] CaptureStage start failed",
                   config_.stream_id.c_str());

        if (debug_stage_) {
            debug_stage_->stop();
        }

        raw_frame_queue_.stop();
        return false;
    }

    return true;
}

void CameraPipeline::destroyStages()
{
    if (capture_stage_) {
        capture_stage_->stop();
        capture_stage_.reset();
    }

    raw_frame_queue_.stop();

    if (debug_stage_) {
        debug_stage_->stop();
        debug_stage_.reset();
    }
}

} // namespace rkcam