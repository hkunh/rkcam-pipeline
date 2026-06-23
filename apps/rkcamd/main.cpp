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

    /*
     * 当前 RGA 输入需要 V4L2 dma_fd。
     */
    config.capture_stage_config.source.export_dma_fd = true;
    config.capture_stage_config.output_memory_type =
        rkcam::VideoMemoryType::DmaBuffer;

    /*
     * 测试阶段只跑 100 帧。
     * 停止条件放在 CaptureStage，而不是 RawSaveStage。
     */
    config.capture_stage_config.max_frames = 100;

    /*
     * CaptureStage -> RgaStage
     *
     * 测试阶段建议 Block，避免丢帧导致保存帧数不好判断。
     */
    config.raw_queue_capacity = 4;
    config.raw_queue_policy = rkcam::QueueFullPolicy::Block;

    /*
     * RgaStage:
     *   input : V4L2 DmaBuffer NV12 1920x1080
     *   output: MPP Buffer DmaBuffer NV12 640x360
     *
     * 注意：
     *   request 输出尺寸/格式必须和 mpp_buffer_pool 保持一致。
     */
    config.rga_stage_config.stage_name = "rga";

    config.rga_stage_config.request.output_width = 640;
    config.rga_stage_config.request.output_height = 360;
    config.rga_stage_config.request.output_format = rkcam::PixelFormat::NV12;
    config.rga_stage_config.request.output_memory_type =
        rkcam::VideoMemoryType::DmaBuffer;
    config.rga_stage_config.request.allow_fallback = false;

    /*
     * 关键：RGA 输出 buffer 由 MPP buffer pool 提供。
     */
    config.rga_stage_config.output_pool_type = rkcam::RgaOutputPoolType::Mpp;

    config.rga_stage_config.mpp_buffer_pool.pool_name = "rga_mpp_pool";
    config.rga_stage_config.mpp_buffer_pool.width = 640;
    config.rga_stage_config.mpp_buffer_pool.height = 360;
    config.rga_stage_config.mpp_buffer_pool.format = rkcam::PixelFormat::NV12;
    config.rga_stage_config.mpp_buffer_pool.buffer_count = 4;

    /*
     * 0 表示由 MppBufferPool 自动按 align 计算。
     *
     * 640 按 16 对齐仍是 640。
     * 360 按 16 对齐会变成 368。
     *
     * 所以你的 VideoBufferPlane 里应该有：
     *   stride = 640
     *   height_stride = 368
     *
     * RawSaveStage 保存 tight NV12 时：
     *   Y 只保存 visible height = 360 行
     *   UV 起始位置要用 height_stride = 368
     */
    config.rga_stage_config.mpp_buffer_pool.hor_stride = 0;
    config.rga_stage_config.mpp_buffer_pool.ver_stride = 0;
    config.rga_stage_config.mpp_buffer_pool.stride_align = 16;
    config.rga_stage_config.mpp_buffer_pool.height_align = 16;

    /*
     * 如果你的 MppBufferPoolConfig 默认就是 MPP_BUFFER_TYPE_DRM，
     * 这行可以不写。
     *
     * 如果头文件里能直接看到 MPP_BUFFER_TYPE_DRM，也可以显式写：
     */
    config.rga_stage_config.mpp_buffer_pool.buffer_type = MPP_BUFFER_TYPE_DRM;

    /*
     * RgaStage -> RawSaveStage
     */
    config.processed_queue_capacity = 4;
    config.processed_queue_policy = rkcam::QueueFullPolicy::Block;

    /*
     * DebugStage:
     *   保存 RGA 写入后的 MPP buffer。
     *
     * 注意：
     *   RawSaveStage 需要 frame.buffer->planes[0].data 有效。
     *   也就是 MPP buffer 能通过 mpp_buffer_get_ptr() 拿到 CPU 地址。
     */
    config.debug_stage = rkcam::CameraDebugStage::RawSave;

    config.raw_save.stage_name = "raw_save";
    config.raw_save.output_path =
        "/userdata/rkcam/output/rga_mppbuf_640x360_nv12.yuv";
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