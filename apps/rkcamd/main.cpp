#include "rkcam/core/log.hpp"
#include "rkcam/pipeline/camera_pipeline.hpp"

#include <chrono>
#include <csignal>
#include <thread>

static volatile std::sig_atomic_t g_running = 1;

static void signalHandler(int)
{
    g_running = 0;
}

int main()
{
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    rkcam::CameraPipelineConfig config;

    config.stream_id = "cam0";

    /*
     * CaptureStage:
     *   V4L2 采集 1920x1080 NV12
     *   输出 DmaBuffer
     *   最多采 100 帧
     */
    config.capture_stage_config.stream_id = "cam0";
    config.capture_stage_config.source.device = "/dev/video0";
    config.capture_stage_config.source.width = 1920;
    config.capture_stage_config.source.height = 1080;
    config.capture_stage_config.source.pixel_format = "NV12";
    config.capture_stage_config.source.buffer_count = 4;
    config.capture_stage_config.source.export_dma_fd = true;

    config.capture_stage_config.output_memory_type =
        rkcam::VideoMemoryType::DmaBuffer;

    config.capture_stage_config.max_frames = 100;

    /*
     * raw_queue:
     *   CaptureStage -> RgaStage
     *
     * 测试阶段建议 Block，避免 RGA 慢一点时丢帧。
     * 等验证稳定后，再改 DropOldest 做实时策略。
     */
    config.raw_queue_capacity = 4;
    config.raw_queue_policy = rkcam::QueueFullPolicy::Block;

    /*
     * RgaStage:
     *   input : DmaBuffer NV12 1920x1080
     *   output: Cpu NV12 640x360
     */
    config.rga_stage_config.stage_name = "rga";
    config.rga_stage_config.request.output_width = 640;
    config.rga_stage_config.request.output_height = 360;
    config.rga_stage_config.request.output_format = rkcam::PixelFormat::NV12;
    config.rga_stage_config.request.output_memory_type =
        rkcam::VideoMemoryType::Cpu;
    config.rga_stage_config.request.allow_fallback = false;

    /*
     * processed_queue:
     *   RgaStage -> RawSaveStage
     */
    config.processed_queue_capacity = 4;
    config.processed_queue_policy = rkcam::QueueFullPolicy::Block;

    /*
     * DebugStage:
     *   当前 RawSaveStage 保存的是 RGA 输出后的 640x360 NV12，
     *   不是原始 1920x1080。
     */
    config.debug_stage = rkcam::CameraDebugStage::RawSave;

    config.raw_save.stage_name = "raw_save";
    config.raw_save.output_path =
        "/userdata/rkcam/output/rga_pipeline_640x360_nv12.yuv";
    config.raw_save.save_tight_nv12 = true;

    rkcam::CameraPipeline pipeline(config);

    if (!pipeline.start()) {
        RKCAM_LOGE("CameraPipeline start failed");
        return 1;
    }

    while (g_running && pipeline.isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    pipeline.stop();

    RKCAM_LOGI("rkcamd exit");
    return 0;
}