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
     * 队列配置。
     *
     * 注意：
     *   相邻 node 使用相同 queue name，
     *   CameraPipeline::createOrReuseQueue() 会让它们复用同一个队列对象。
     */
    rkcam::PipelineQueueConfig q_cap_to_rga;
    q_cap_to_rga.name = "cap_to_rga";
    q_cap_to_rga.capacity = 4;
    q_cap_to_rga.policy = rkcam::QueueFullPolicy::Block;

    rkcam::PipelineQueueConfig q_rga_to_save;
    q_rga_to_save.name = "rga_to_save";
    q_rga_to_save.capacity = 4;
    q_rga_to_save.policy = rkcam::QueueFullPolicy::Block;

    /*
     * CaptureStage:
     *   V4L2 采集 1920x1080 NV12
     *   输出 DmaBuffer
     *   最多采集 100 帧
     */
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

    /*
     * RgaStage:
     *   input : 1920x1080 NV12 DmaBuffer
     *   output: 640x360 NV12 Cpu
     *
     * 第一版先用 CPU 输出，方便 RawSave 验证。
     */
    rkcam::RgaStageConfig rga_cfg;
    rga_cfg.stage_name = "rga";

    rga_cfg.request.output_width = 640;
    rga_cfg.request.output_height = 360;
    rga_cfg.request.output_format = rkcam::PixelFormat::NV12;
    rga_cfg.request.output_memory_type = rkcam::VideoMemoryType::Cpu;
    rga_cfg.request.allow_fallback = false;

    /*
     * RawSaveStage:
     *   保存 RGA 输出后的 640x360 NV12。
     */
    rkcam::RawSaveStageConfig save_cfg;
    save_cfg.stage_name = "raw_save";
    save_cfg.output_path =
        "/userdata/rkcam/output/stagenode_rga_640x360_nv12.yuv";
    save_cfg.save_tight_nv12 = true;

    /*
     * StageNode 1: Capture
     */
    rkcam::StageNodeConfig capture_node;
    capture_node.name = "capture";
    capture_node.type = rkcam::StageType::Capture;
    capture_node.output_queue = q_cap_to_rga;
    capture_node.capture = capture_cfg;

    /*
     * StageNode 2: RGA
     */
    rkcam::StageNodeConfig rga_node;
    rga_node.name = "rga";
    rga_node.type = rkcam::StageType::Rga;
    rga_node.input_queue = q_cap_to_rga;
    rga_node.output_queue = q_rga_to_save;
    rga_node.rga = rga_cfg;

    /*
     * StageNode 3: RawSave
     */
    rkcam::StageNodeConfig save_node;
    save_node.name = "raw_save";
    save_node.type = rkcam::StageType::RawSave;
    save_node.input_queue = q_rga_to_save;
    save_node.raw_save = save_cfg;

    /*
     * nodes 按 上游 -> 下游 顺序填写。
     * CameraPipeline 内部 start 时会反过来启动：
     *   RawSave -> RGA -> Capture
     *
     * stop 时按这个顺序停止：
     *   Capture -> RGA -> RawSave
     */
    config.nodes = {
        capture_node,
        rga_node,
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