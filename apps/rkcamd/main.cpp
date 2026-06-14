#include "rkcam/core/log.hpp"
#include "rkcam/pipeline/camera_pipeline.hpp"

#include <chrono>
#include <csignal>
#include <thread>

static bool g_running = true;

static void signalHandler(int)
{
    g_running = false;
}

int main()
{
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    rkcam::CameraPipelineConfig config;

    config.stream_id = "cam0";

    config.source.device = "/dev/video0";
    config.source.width = 1920;
    config.source.height = 1080;
    config.source.pixel_format = "NV12";
    config.source.buffer_count = 4;

    config.capture_output_memory_type = rkcam::VideoMemoryType::Cpu;

    config.raw_queue_capacity = 4;
    config.raw_queue_policy = rkcam::QueueFullPolicy::DropOldest;

    // config.debug_stage = rkcam::CameraDebugStage::Fps;
    // config.fps.stage_name = "fps";
    // config.fps.print_interval_sec = 1.0;

    config.debug_stage = rkcam::CameraDebugStage::RawSave;

    config.raw_save.stage_name = "raw_save";
    config.raw_save.output_path = "/userdata/rkcam/output/pipeline_capture_1920x1080_nv12.yuv";
    config.raw_save.max_frames = 100;
    config.raw_save.save_tight_nv12 = true;
    config.raw_save.stop_queue_when_done = true;

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