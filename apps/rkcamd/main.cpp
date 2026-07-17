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
     * ============================================================
     * 0. 基础参数
     * ============================================================
     */

    const int capture_width = 1920;
    const int capture_height = 1080;

    /*
     * 预览分支：
     *   MIPI 屏幕是 1080x1920。
     *   这里先用小窗口测试。
     *
     * 如果要全屏，改成：
     *   preview_width  = 1080;
     *   preview_height = 1920;
     */
    const int preview_width = 360;
    const int preview_height = 640;

    /*
     * 录像分支：
     *   第一版先录 640x360 H264 MP4。
     */
    const int record_width = 640;
    const int record_height = 360;

    const int record_hor_stride = alignTo(record_width, 16);
    const int record_ver_stride = alignTo(record_height, 16);

    const int fps = 30;
    const int bitrate = 2 * 1000 * 1000;
    const int gop = 30;

    /*
     * ============================================================
     * 1. 队列定义
     * ============================================================
     */

    /*
     * Capture -> Tee
     */
    rkcam::PipelineQueueConfig cap_to_tee;
    cap_to_tee.name = "cap_to_tee";
    cap_to_tee.value_type = rkcam::PipelineQueueValueType::PipelineVideoFrame;
    cap_to_tee.capacity = 2;
    cap_to_tee.policy = rkcam::QueueFullPolicy::DropOldest;

    /*
     * Tee -> PreviewRga
     *
     * 预览低延迟优先，队列不要太大。
     */
    rkcam::PipelineQueueConfig tee_to_preview;
    tee_to_preview.name = "tee_to_preview";
    tee_to_preview.value_type = rkcam::PipelineQueueValueType::PipelineVideoFrame;
    tee_to_preview.capacity = 1;
    tee_to_preview.policy = rkcam::QueueFullPolicy::DropOldest;

    /*
     * Tee -> RecordRga
     *
     * 录像分支可以稍微大一点。
     */
    rkcam::PipelineQueueConfig tee_to_record;
    tee_to_record.name = "tee_to_record";
    tee_to_record.value_type = rkcam::PipelineQueueValueType::PipelineVideoFrame;
    tee_to_record.capacity = 4;
    tee_to_record.policy = rkcam::QueueFullPolicy::DropOldest;

    /*
     * PreviewRga -> Display
     */
    rkcam::PipelineQueueConfig preview_to_display;
    preview_to_display.name = "preview_to_display";
    preview_to_display.value_type = rkcam::PipelineQueueValueType::PipelineVideoFrame;
    preview_to_display.capacity = 2;
    preview_to_display.policy = rkcam::QueueFullPolicy::DropOldest;

    /*
     * RecordRga -> Mpp
     */
    rkcam::PipelineQueueConfig record_to_mpp;
    record_to_mpp.name = "record_to_mpp";
    record_to_mpp.value_type = rkcam::PipelineQueueValueType::PipelineVideoFrame;
    record_to_mpp.capacity = 4;
    record_to_mpp.policy = rkcam::QueueFullPolicy::DropOldest;

    /*
     * Mpp -> EncodedPacketTee
     */
    rkcam::PipelineQueueConfig mpp_to_encodedpacket_tee;
    mpp_to_encodedpacket_tee.name = "mpp_to_encodedpacket_tee";
    mpp_to_encodedpacket_tee.value_type = rkcam::PipelineQueueValueType::EncodedPacket;
    mpp_to_encodedpacket_tee.capacity = 16;
    mpp_to_encodedpacket_tee.policy = rkcam::QueueFullPolicy::DropOldest;

    /*
     * ============================================================
     * EncodedPacketTee->Mp4Record
     * ============================================================
     */
    rkcam::PipelineQueueConfig encodedpacket_tee_to_mp4record;
    encodedpacket_tee_to_mp4record.name = "encodedpacket_tee_to_mp4record";
    encodedpacket_tee_to_mp4record.value_type = rkcam::PipelineQueueValueType::EncodedPacket;
    encodedpacket_tee_to_mp4record.capacity = 16;
    encodedpacket_tee_to_mp4record.policy = rkcam::QueueFullPolicy::DropOldest;

    /*
     * ============================================================
     * EncodedPacketTee->rtspPush
     * ============================================================
     */
    rkcam::PipelineQueueConfig encodedpacket_tee_to_rtsppush;
    encodedpacket_tee_to_rtsppush.name = "encodedpacket_tee_to_rtsppush";
    encodedpacket_tee_to_rtsppush.value_type = rkcam::PipelineQueueValueType::EncodedPacket;
    encodedpacket_tee_to_rtsppush.capacity = 16;
    encodedpacket_tee_to_rtsppush.policy = rkcam::QueueFullPolicy::DropOldest;




    /*
     * ============================================================
     * 2. CaptureStage
     * ============================================================
     */

    rkcam::StageNodeConfig capture;
    capture.name = "capture";
    capture.type = rkcam::StageType::Capture;
    capture.output_queues = {
        cap_to_tee,
    };

    capture.capture.stream_id = "cam0";
    capture.capture.max_frames = 0;
    capture.capture.output_memory_type = rkcam::VideoMemoryType::DmaBuffer;

    capture.capture.source.device = "/dev/video0";
    capture.capture.source.width = capture_width;
    capture.capture.source.height = capture_height;
    capture.capture.source.pixel_format = "NV12";

    /*
     * 双分支时，V4L2 buffer_count 建议比单分支稍大。
     * 如果你的驱动分配失败，再改回 4。
     */
    capture.capture.source.buffer_count = 6;
    capture.capture.source.export_dma_fd = true;

    /*
     * ============================================================
     * 3. VideoFrameTeeStage
     * ============================================================
     */

    rkcam::StageNodeConfig tee;
    tee.name = "video_tee";
    tee.type = rkcam::StageType::VideoFrameTee;
    tee.input_queue = cap_to_tee;
    tee.output_queues = {
        tee_to_preview,
        tee_to_record,
    };

    tee.video_frame_tee.stage_name = "video_tee";
    tee.video_frame_tee.continue_on_output_fail = true;
    tee.video_frame_tee.log_interval = 30;

    /*
     * ============================================================
     * 4. Preview RGA 分支：Tee -> PreviewRga -> Display
     * ============================================================
     */

    rkcam::StageNodeConfig preview_rga;
    preview_rga.name = "preview_rga";
    preview_rga.type = rkcam::StageType::Rga;
    preview_rga.input_queue = tee_to_preview;
    preview_rga.output_queues = {
        preview_to_display,
    };

    preview_rga.rga.stage_name = "preview_rga";

    preview_rga.rga.request.output_width = preview_width;
    preview_rga.rga.request.output_height = preview_height;
    preview_rga.rga.request.output_format = rkcam::PixelFormat::NV12;
    preview_rga.rga.request.output_memory_type = rkcam::VideoMemoryType::DmaBuffer;

    /*
     * 摄像头是 1920x1080 横屏，MIPI 是 1080x1920 竖屏。
     * 如果方向反了，Rotate270 改成 Rotate90。
     */
    preview_rga.rga.request.rotate = rkcam::RgaRotateMode::Rotate270;

    preview_rga.rga.output_pool_type = rkcam::RgaOutputPoolType::Drm;

    preview_rga.rga.drm_buffer_pool.pool_name = "preview_drm_pool";
    preview_rga.rga.drm_buffer_pool.device = "/dev/dri/card0";
    preview_rga.rga.drm_buffer_pool.width = preview_width;
    preview_rga.rga.drm_buffer_pool.height = preview_height;
    preview_rga.rga.drm_buffer_pool.format = rkcam::PixelFormat::NV12;
    preview_rga.rga.drm_buffer_pool.buffer_count = 4;
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
    display.drm_display.width = preview_width;
    display.drm_display.height = preview_height;
    display.drm_display.format = rkcam::PixelFormat::NV12;
    display.drm_display.hold_frame_count = 2;
    display.drm_display.strict_frame_size = true;

    /*
     * 小窗口居中显示。
     *
     * 屏幕 DRM mode 是 1080x1920。
     */
    display.drm_display.dst_rect.x = (1080 - preview_width) / 2;
    display.drm_display.dst_rect.y = (1920 - preview_height) / 2;
    display.drm_display.dst_rect.width = preview_width;
    display.drm_display.dst_rect.height = preview_height;

    display.drm_display.drm.device = "/dev/dri/card0";
    display.drm_display.drm.connector_id = 150;
    display.drm_display.drm.crtc_id = 85;
    display.drm_display.drm.plane_id = 101;
    display.drm_display.drm.format = rkcam::PixelFormat::NV12;
    display.drm_display.drm.clear_plane_on_close = true;

    /*
     * ============================================================
     * 5. Record RGA 分支：Tee -> RecordRga -> Mpp -> Mp4Record
     * ============================================================
     */

    rkcam::StageNodeConfig record_rga;
    record_rga.name = "record_rga";
    record_rga.type = rkcam::StageType::Rga;
    record_rga.input_queue = tee_to_record;
    record_rga.output_queues = {
        record_to_mpp,
    };

    record_rga.rga.stage_name = "record_rga";

    /*
     * 录像默认保持横屏 640x360，不做 rotate。
     * 如果你想录像也是竖屏，再加 Rotate270，并把尺寸改成 360x640。
     */
    record_rga.rga.request.output_width = record_width;
    record_rga.rga.request.output_height = record_height;
    record_rga.rga.request.output_format = rkcam::PixelFormat::NV12;
    record_rga.rga.request.output_memory_type = rkcam::VideoMemoryType::DmaBuffer;
    record_rga.rga.request.rotate = rkcam::RgaRotateMode::None;

    record_rga.rga.output_pool_type = rkcam::RgaOutputPoolType::Mpp;

    record_rga.rga.mpp_buffer_pool.pool_name = "record_mpp_pool";
    record_rga.rga.mpp_buffer_pool.width = record_width;
    record_rga.rga.mpp_buffer_pool.height = record_height;
    record_rga.rga.mpp_buffer_pool.hor_stride = record_hor_stride;
    record_rga.rga.mpp_buffer_pool.ver_stride = record_ver_stride;
    record_rga.rga.mpp_buffer_pool.format = rkcam::PixelFormat::NV12;
    record_rga.rga.mpp_buffer_pool.buffer_count = 4;


    /*
     * ============================================================
     * 6. MppStage
     * ============================================================
     */

    rkcam::StageNodeConfig mpp;
    mpp.name = "mpp";
    mpp.type = rkcam::StageType::Mpp;
    mpp.input_queue = record_to_mpp;
    mpp.output_queues = {
        mpp_to_encodedpacket_tee,
    };

    mpp.mpp.stage_name = "mpp";

    mpp.mpp.encoder.width = record_width;
    mpp.mpp.encoder.height = record_height;
    mpp.mpp.encoder.hor_stride = record_hor_stride;
    mpp.mpp.encoder.ver_stride = record_ver_stride;
    mpp.mpp.encoder.input_format = rkcam::PixelFormat::NV12;
    mpp.mpp.encoder.codec = rkcam::CodecType::H264;
    mpp.mpp.encoder.fps = fps;
    mpp.mpp.encoder.gop = gop;
    mpp.mpp.encoder.bitrate = bitrate;


    /*
     * ============================================================
     * 7. Mp4RecordStage
     * ============================================================
     */

    rkcam::StageNodeConfig mp4_record;
    mp4_record.name = "mp4_record";
    mp4_record.type = rkcam::StageType::Mp4Record;
    mp4_record.input_queue = encodedpacket_tee_to_mp4record;

    mp4_record.mp4_record.stage_name = "mp4_record";
    mp4_record.mp4_record.output_path =
        "/userdata/rkcam/output/preview_record_test.mp4";

    mp4_record.mp4_record.width = record_width;
    mp4_record.mp4_record.height = record_height;
    mp4_record.mp4_record.fps = fps;
    mp4_record.mp4_record.codec = rkcam::CodecType::H264;


    /*
     * ============================================================
     * 8. EncodedPacketTeeStageConfig
     * ============================================================
     */
    rkcam::StageNodeConfig encodedPacket_tee_config;

    encodedPacket_tee_config.name = "encoded_packet_tee";
    encodedPacket_tee_config.type = rkcam::StageType::EncodedPacketTee;
    encodedPacket_tee_config.input_queue = mpp_to_encodedpacket_tee;
    encodedPacket_tee_config.output_queues = {
        encodedpacket_tee_to_mp4record,
        encodedpacket_tee_to_rtsppush,
    };

    encodedPacket_tee_config.encoded_packet_tee.stage_name = "encoded_packet_tee";
    encodedPacket_tee_config.encoded_packet_tee.continue_on_output_fail = true;
    encodedPacket_tee_config.encoded_packet_tee.log_interval = 30;

    /*
     * ============================================================
     * 9. RtspPushStageConfig
     * ============================================================
     */
    rkcam::RtspVideoStreamConfig rtsp_video_cfg;
    rtsp_video_cfg.enabled = true;
    rtsp_video_cfg.codec = rkcam::CodecType::H264;
    rtsp_video_cfg.width = record_width;
    rtsp_video_cfg.height = record_height;
    rtsp_video_cfg.fps = 30;
    rtsp_video_cfg.wait_key_frame = true;

    rkcam::RtspAudioStreamConfig rtsp_audio_cfg;
    rtsp_audio_cfg.enabled = false; 

    rkcam::StageNodeConfig rtsp_push_cfg;
    rtsp_push_cfg.name = "rtsp_push";
    rtsp_push_cfg.type = rkcam::StageType::RtspPush;
    rtsp_push_cfg.input_queue = encodedpacket_tee_to_rtsppush;

    rtsp_push_cfg.rtsp_push.stage_name = "rtsp_push";
    rtsp_push_cfg.rtsp_push.url = "rtsp://192.168.1.100:8554/live";
    rtsp_push_cfg.rtsp_push.video = rtsp_video_cfg;
    rtsp_push_cfg.rtsp_push.audio = rtsp_audio_cfg;
    rtsp_push_cfg.rtsp_push.rtsp_over_tcp = true;


    /*
     * ============================================================
     * 节点顺序
     *
     * 注意：
     *   按上游到下游写。
     *   如果你的 CameraPipeline::startStages() 是反向启动，
     *   下游会先启动，上游最后启动，这是最理想的。
     * ============================================================
     */

    cfg.nodes = {
        capture,
        tee,

        preview_rga,
        display,

        record_rga,
        mpp,
        mp4_record,
    };

    rkcam::CameraPipeline pipeline(cfg);

    if (!pipeline.start()) {
        RKCAM_LOGE("CameraPipeline start failed");
        return 1;
    }

    RKCAM_LOGI("rkcamd running: preview + record. Press Ctrl+C to stop.");

    /*
     * 跑 30 秒，也可以 Ctrl+C 提前停止。
     */
    const int run_seconds = 30;

    for (int i = 0; i < run_seconds && g_running; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    RKCAM_LOGI("rkcamd stopping...");

    pipeline.stop();

    RKCAM_LOGI("rkcamd exit");
    return 0;
}