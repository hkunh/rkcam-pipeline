/**
*@file main.cpp
*@brief 
*@author HKH368
*@date 2026-05-29
*@return 
*/

#include "rkcam/core/blocking_queue.hpp"
#include "rkcam/core/log.hpp"
#include "rkcam/pipeline/capture_stage.hpp"
#include "rkcam/pipeline/fps_stage.hpp"
#include "rkcam/video/v4l2_video_source.hpp"


#include <chrono>
#include <csignal>
#include <cstdlib>
#include <string>

bool g_running = true;
static void signalHandler(int)
{
    g_running = false;
}


int main()
{
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    rkcam::BlockingQueue<rkcam::PipelineVideoFrame> raw_queue(
        4,
        rkcam::QueueFullPolicy::DropOldest);

    rkcam::V4L2VideoSourceConfig source_cfg;
    source_cfg.device = "/dev/video0";
    source_cfg.width = 1920;
    source_cfg.height = 1080;
    source_cfg.pixel_format = "NV12";
    source_cfg.buffer_count = 4;

    auto source = std::make_unique<rkcam::V4L2VideoSource>(source_cfg);

    rkcam::CaptureStageConfig capture_cfg;
    capture_cfg.stream_id = "cam0";
    capture_cfg.output_memory_type = rkcam::VideoMemoryType::Cpu;

    rkcam::FpsStageConfig fps_cfg;
    fps_cfg.stage_name = "fps";
    fps_cfg.print_interval_sec = 1.0;

    rkcam::FpsStage fps_stage(fps_cfg, raw_queue);
    rkcam::CaptureStage capture_stage(
        capture_cfg,
        std::move(source),
        raw_queue);

    if (!fps_stage.start()) {
        RKCAM_LOGE("fps_stage start failed");
        return 1;
    }

    if (!capture_stage.start()) {
        RKCAM_LOGE("capture_stage start failed");
        raw_queue.stop();
        fps_stage.stop();
        return 1;
    }

    RKCAM_LOGI("pipeline test started");

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    RKCAM_LOGI("pipeline test stopping");

    capture_stage.stop();
    raw_queue.stop();
    fps_stage.stop();

    RKCAM_LOGI("pipeline test exit");

    return 0;
}
