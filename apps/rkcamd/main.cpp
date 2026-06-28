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

    rkcam::CameraPipelineConfig config;
    config.stream_id = "cam0";

    rkcam::PipelineQueueConfig q_cap_to_rga;
    q_cap_to_rga.name = "cap_to_rga";
    q_cap_to_rga.value_type = rkcam::PipelineQueueValueType::PipelineVideoFrame;
    q_cap_to_rga.capacity = 4;
    q_cap_to_rga.policy = rkcam::QueueFullPolicy::Block;

    rkcam::PipelineQueueConfig q_rga_to_mpp;
    q_rga_to_mpp.name = "rga_to_mpp";
    q_rga_to_mpp.value_type = rkcam::PipelineQueueValueType::PipelineVideoFrame;
    q_rga_to_mpp.capacity = 4;
    q_rga_to_mpp.policy = rkcam::QueueFullPolicy::Block;

    rkcam::PipelineQueueConfig q_mpp_to_save;
    q_mpp_to_save.name = "mpp_to_save";
    q_mpp_to_save.value_type = rkcam::PipelineQueueValueType::EncodedPacket;
    q_mpp_to_save.capacity = 8;
    q_mpp_to_save.policy = rkcam::QueueFullPolicy::Block;

    rkcam::CaptureStageConfig capture_cfg;
    capture_cfg.stream_id = "cam0";
    capture_cfg.source.device = "/dev/video0";
    capture_cfg.source.width = 1920;
    capture_cfg.source.height = 1080;
    capture_cfg.source.pixel_format = "NV12";
    capture_cfg.source.buffer_count = 4;
    capture_cfg.source.export_dma_fd = true;
    capture_cfg.output_memory_type = rkcam::VideoMemoryType::DmaBuffer;
    capture_cfg.max_frames = 100;

    rkcam::RgaStageConfig rga_cfg;
    rga_cfg.stage_name = "rga";
    rga_cfg.request.output_width = 640;
    rga_cfg.request.output_height = 360;
    rga_cfg.request.output_format = rkcam::PixelFormat::NV12;
    rga_cfg.request.output_memory_type = rkcam::VideoMemoryType::DmaBuffer;
    rga_cfg.request.allow_fallback = false;

    rga_cfg.output_pool_type = rkcam::RgaOutputPoolType::Mpp;
    rga_cfg.mpp_buffer_pool.pool_name = "rga_mpp_pool";
    rga_cfg.mpp_buffer_pool.width = 640;
    rga_cfg.mpp_buffer_pool.height = 360;
    rga_cfg.mpp_buffer_pool.format = rkcam::PixelFormat::NV12;
    rga_cfg.mpp_buffer_pool.buffer_count = 4;
    rga_cfg.mpp_buffer_pool.hor_stride = 640;
    rga_cfg.mpp_buffer_pool.ver_stride = alignTo(360, 16);

    rkcam::MppStageConfig mpp_cfg;
    mpp_cfg.stage_name = "mpp";
    mpp_cfg.encoder.width = 640;
    mpp_cfg.encoder.height = 360;
    mpp_cfg.encoder.input_format = rkcam::PixelFormat::NV12;
    mpp_cfg.encoder.hor_stride = 0;
    mpp_cfg.encoder.ver_stride = 0;
    mpp_cfg.encoder.codec = rkcam::CodecType::H264;
    mpp_cfg.encoder.fps = 30;
    mpp_cfg.encoder.gop = 30;
    mpp_cfg.encoder.bitrate = 2 * 1000 * 1000;

    rkcam::EncodedSaveStageConfig encoded_save_cfg;
    encoded_save_cfg.stage_name = "encoded_save";
    encoded_save_cfg.output_path =
        "/userdata/rkcam/output/pipeline_mpp_640x360.h264";
    encoded_save_cfg.max_packets = 0;

    rkcam::StageNodeConfig capture_node;
    capture_node.name = "capture";
    capture_node.type = rkcam::StageType::Capture;
    capture_node.output_queue = q_cap_to_rga;
    capture_node.capture = capture_cfg;

    rkcam::StageNodeConfig rga_node;
    rga_node.name = "rga";
    rga_node.type = rkcam::StageType::Rga;
    rga_node.input_queue = q_cap_to_rga;
    rga_node.output_queue = q_rga_to_mpp;
    rga_node.rga = rga_cfg;

    rkcam::StageNodeConfig mpp_node;
    mpp_node.name = "mpp";
    mpp_node.type = rkcam::StageType::Mpp;
    mpp_node.input_queue = q_rga_to_mpp;
    mpp_node.output_queue = q_mpp_to_save;
    mpp_node.mpp = mpp_cfg;

    rkcam::StageNodeConfig save_node;
    save_node.name = "encoded_save";
    save_node.type = rkcam::StageType::EncodedSave;
    save_node.input_queue = q_mpp_to_save;
    save_node.encoded_save = encoded_save_cfg;

    config.nodes = {
        capture_node,
        rga_node,
        mpp_node,
        save_node,
    };

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