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

static int alignTo(int value, int align)
{
    if (value <= 0 || align <= 0) {
        return 0;
    }

    return (value + align - 1) / align * align;
}

int main()
{
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    rkcam::CameraPipelineConfig cfg;
    cfg.stream_id = "cam0";

    /*
    * queue: capture -> preview_rga
    */
    rkcam::PipelineQueueConfig cap_to_preview;
    cap_to_preview.name = "cap_to_preview";
    cap_to_preview.value_type = rkcam::PipelineQueueValueType::PipelineVideoFrame;
    cap_to_preview.capacity = 2;
    cap_to_preview.policy = rkcam::QueueFullPolicy::DropOldest;

    /*
    * queue: preview_rga -> display
    */
    rkcam::PipelineQueueConfig preview_to_display;
    preview_to_display.name = "preview_to_display";
    preview_to_display.value_type = rkcam::PipelineQueueValueType::PipelineVideoFrame;
    preview_to_display.capacity = 2;
    preview_to_display.policy = rkcam::QueueFullPolicy::DropOldest;

    /*
    * Capture
    */
    rkcam::StageNodeConfig capture;
    capture.name = "capture";
    capture.type = rkcam::StageType::Capture;
    capture.output_queue = cap_to_preview;

    capture.capture.stream_id = "cam0";
    capture.capture.max_frames = 0;
    capture.capture.output_memory_type = rkcam::VideoMemoryType::DmaBuffer;

    capture.capture.source.device = "/dev/video0";
    capture.capture.source.width = 1920;
    capture.capture.source.height = 1080;
    capture.capture.source.pixel_format = "NV12";
    capture.capture.source.buffer_count = 4;
    capture.capture.source.export_dma_fd = true;

    /*
    * RGA preview
    *
    * 第一版先只 resize 到 1080x1920。 widthxheight
    * 如果画面被拉伸，下一步再加 rotate/crop/keep_aspect。
    */
    rkcam::StageNodeConfig preview_rga;
    preview_rga.name = "preview_rga";
    preview_rga.type = rkcam::StageType::Rga;
    preview_rga.input_queue = cap_to_preview;
    preview_rga.output_queue = preview_to_display;

    preview_rga.rga.stage_name = "preview_rga";

    int temp_width = 360;
    int temp_height = 720;

    preview_rga.rga.request.output_width = temp_width;
    preview_rga.rga.request.output_height = temp_height;
    preview_rga.rga.request.output_format = rkcam::PixelFormat::NV12;
    preview_rga.rga.request.output_memory_type = rkcam::VideoMemoryType::DmaBuffer;
    preview_rga.rga.request.rotate = rkcam::RgaRotateMode::Rotate270;

    preview_rga.rga.output_pool_type = rkcam::RgaOutputPoolType::Drm;

    preview_rga.rga.drm_buffer_pool.pool_name = "preview_drm_pool";
    preview_rga.rga.drm_buffer_pool.device = "/dev/dri/card0";
    preview_rga.rga.drm_buffer_pool.width = temp_width;
    preview_rga.rga.drm_buffer_pool.height = temp_height;
    preview_rga.rga.drm_buffer_pool.format = rkcam::PixelFormat::NV12;
    preview_rga.rga.drm_buffer_pool.buffer_count = 4;

    /*
    * RGA 直接写 dma_fd，不需要 CPU mmap。
    * 但如果你想调试保存 DRM buffer 内容，可以临时设 true。
    */
    preview_rga.rga.drm_buffer_pool.map_cpu = false;
    preview_rga.rga.drm_buffer_pool.clear_on_init = true;

    /*
    * Display
    */
    rkcam::StageNodeConfig display;
    display.name = "display";
    display.type = rkcam::StageType::Display;
    display.input_queue = preview_to_display;

    display.display.stage_name = "display";
    display.display.max_failed_frames = 30;

    display.drm_display.sink_name = "drm_display";
    display.drm_display.width = temp_width;
    display.drm_display.height = temp_height;
    display.drm_display.format = rkcam::PixelFormat::NV12;
    display.drm_display.hold_frame_count = 2;
    display.drm_display.strict_frame_size = true;

    display.drm_display.dst_rect.x = 720;
    display.drm_display.dst_rect.y = 360;
    display.drm_display.dst_rect.width = temp_width;
    display.drm_display.dst_rect.height = temp_height;

    display.drm_display.drm.device = "/dev/dri/card0";
    display.drm_display.drm.connector_id = 150;
    display.drm_display.drm.crtc_id = 85;
    display.drm_display.drm.plane_id = 101;
    display.drm_display.drm.format = rkcam::PixelFormat::NV12;
    display.drm_display.drm.clear_plane_on_close = true;

    cfg.nodes = {
        capture,
        preview_rga,
        display,
    };

    rkcam::CameraPipeline pipeline(cfg);

    if (!pipeline.start()) {
        RKCAM_LOGE("CameraPipeline start failed");
        return 1;
    }

    sleep(30);

    pipeline.stop();
    return 0;
}