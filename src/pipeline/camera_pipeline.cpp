#include "rkcam/pipeline/camera_pipeline.hpp"
#include "rkcam/core/log.hpp"

#include <chrono>
#include <memory>
#include <thread>
#include <utility>

namespace rkcam {

CameraPipeline::CameraPipeline(const CameraPipelineConfig& config)
    : config_(config),
      raw_frame_queue_(config.raw_queue_capacity, config.raw_queue_policy),
      processed_frame_queue_(config.processed_queue_capacity,
                             config.processed_queue_policy)
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
    processed_frame_queue_.reset();

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
    if (!running_ &&
        !capture_stage_ &&
        !rga_stage_ &&
        !debug_stage_) {
        return;
    }

    running_ = false;

    /*
     * 停止顺序：
     *
     * 1. 先停 CaptureStage。
     *    CaptureStage 会 stop raw_frame_queue_，告诉 RgaStage 不会再有新帧。
     *
     * 2. 再停 RgaStage。
     *    RgaStage 应该处理完 raw_frame_queue_ 残留帧，
     *    然后 stop processed_frame_queue_。
     *
     * 3. 最后停 DebugStage。
     *    RawSaveStage/FpsStage 会消费 processed_frame_queue_，
     *    直到 queue stopped 且 empty。
     */
    if (capture_stage_) {
        capture_stage_->stop();
        capture_stage_.reset();
    }

    raw_frame_queue_.stop();

    if (rga_stage_) {
        rga_stage_->stop();
        rga_stage_.reset();
    }

    processed_frame_queue_.stop();

    if (debug_stage_) {
        debug_stage_->stop();
        debug_stage_.reset();
    }

    /*
     * 强制清理残留 frame。
     * 正常 EOF 流程中一般已经为空。
     * 这里主要用于异常 stop，避免 DmaBuffer 残留导致 release_cb 不触发。
     */
    raw_frame_queue_.clear();
    processed_frame_queue_.clear();

    RKCAM_LOGI("[%s] CameraPipeline stopped",
               config_.stream_id.c_str());
}

bool CameraPipeline::isRunning() const
{
    /*
     * 现在不能再只看 raw_frame_queue_。
     *
     * CaptureStage 达到 max_frames 后，raw_frame_queue_ 会 stopped，
     * 但 RgaStage / RawSaveStage 可能还在处理剩余帧。
     *
     * 这里给一个简单判断：
     *   两个队列都 stopped 且都 empty，认为 pipeline 已经自然结束。
     */
    const bool all_queues_done =
        raw_frame_queue_.stopped() &&
        processed_frame_queue_.stopped() &&
        raw_frame_queue_.size() == 0 &&
        processed_frame_queue_.size() == 0;

    return running_ && !all_queues_done;
}

bool CameraPipeline::createStages()
{
    /*
     * 当前先写死：
     *   CaptureStage -> RgaStage -> DebugStage
     */

    CaptureStageConfig capture_config = config_.capture_stage_config;

    /*
     * 当前 RGA 输入需要 DmaBuffer。
     * 所以这里强制打开 dma_fd 导出。
     */
    capture_config.output_memory_type = VideoMemoryType::DmaBuffer;
    capture_config.source.export_dma_fd = true;

    auto source = std::make_unique<V4L2VideoSource>(
        capture_config.source);

    capture_stage_ = std::make_unique<CaptureStage>(
        capture_config,
        std::move(source),
        raw_frame_queue_);

    /*
     * RgaStage:
     *   input : raw_frame_queue_
     *   output: processed_frame_queue_
     */
    RgaStageConfig rga_config = config_.rga_stage_config;
    if (rga_config.stage_name.empty()) {
        rga_config.stage_name = "rga";
    }

    rga_stage_ = std::make_unique<RgaStage>(
        rga_config,
        raw_frame_queue_,
        processed_frame_queue_);

    /*
     * DebugStage 现在消费 RGA 输出后的 processed_frame_queue_。
     */
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
            processed_frame_queue_);
        break;
    }

    case CameraDebugStage::RawSave: {
        RawSaveStageConfig save_config = config_.raw_save;
        if (save_config.stage_name.empty()) {
            save_config.stage_name = "raw_save";
        }

        debug_stage_ = std::make_unique<RawSaveStage>(
            save_config,
            processed_frame_queue_);
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
     * 启动顺序：
     *   下游先启动，上游后启动。
     *
     * RawSave/Fps -> RGA -> Capture
     */

    if (debug_stage_) {
        if (!debug_stage_->start()) {
            RKCAM_LOGE("[%s] debug stage start failed",
                       config_.stream_id.c_str());
            return false;
        }
    }

    if (!rga_stage_) {
        RKCAM_LOGE("[%s] rga_stage_ is null",
                   config_.stream_id.c_str());
        return false;
    }

    if (!rga_stage_->start()) {
        RKCAM_LOGE("[%s] RgaStage start failed",
                   config_.stream_id.c_str());

        if (debug_stage_) {
            debug_stage_->stop();
        }

        processed_frame_queue_.stop();
        return false;
    }

    if (!capture_stage_) {
        RKCAM_LOGE("[%s] capture_stage_ is null",
                   config_.stream_id.c_str());

        if (rga_stage_) {
            rga_stage_->stop();
        }

        if (debug_stage_) {
            debug_stage_->stop();
        }

        return false;
    }

    if (!capture_stage_->start()) {
        RKCAM_LOGE("[%s] CaptureStage start failed",
                   config_.stream_id.c_str());

        if (rga_stage_) {
            rga_stage_->stop();
        }

        if (debug_stage_) {
            debug_stage_->stop();
        }

        raw_frame_queue_.stop();
        processed_frame_queue_.stop();

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

    if (rga_stage_) {
        rga_stage_->stop();
        rga_stage_.reset();
    }

    processed_frame_queue_.stop();

    if (debug_stage_) {
        debug_stage_->stop();
        debug_stage_.reset();
    }

    raw_frame_queue_.clear();
    processed_frame_queue_.clear();
}

} // namespace rkcam