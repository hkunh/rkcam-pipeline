#include "rkcam/core/log.hpp"

#include "rkcam/video/v4l2_video_source.hpp"
#include "rkcam/video/video_frame.hpp"

#include "rkcam/platform/rockchip/rga/rga_processor.hpp"
#include "rkcam/platform/rockchip/mpp/mpp_buffer_pool.hpp"
#include "rkcam/platform/rockchip/mpp/mpp_encoder.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

constexpr const char* kDevice = "/dev/video0";
constexpr const char* kOutputPath =
    "/userdata/rkcam/output/mpp_encoder_640x360.h264";

constexpr int kInputWidth = 1920;
constexpr int kInputHeight = 1080;

constexpr int kOutputWidth = 640;
constexpr int kOutputHeight = 360;

constexpr int kMaxFrames = 100;

int alignTo(int value, int align)
{
    if (value <= 0 || align <= 0) {
        return 0;
    }

    return (value + align - 1) / align * align;
}

bool wrapSourceFrameAsDmaFrame(
    const rkcam::VideoSourceFrame& src,
    rkcam::PipelineVideoFrame& dst,
    const std::string& stream_id)
{
    if (src.planes.empty()) {
        RKCAM_LOGE("wrapSourceFrameAsDmaFrame failed: src.planes is empty");
        return false;
    }

    dst = rkcam::PipelineVideoFrame{};

    dst.stream_id = stream_id;
    dst.width = src.width;
    dst.height = src.height;
    dst.format = src.format;
    dst.pts_us = src.pts_us;
    dst.duration_us = src.duration_us;
    dst.frame_id = src.frame_id;

    auto buffer = std::make_shared<rkcam::VideoBuffer>();
    buffer->memory_type = rkcam::VideoMemoryType::DmaBuffer;
    buffer->planes.resize(src.planes.size());

    for (size_t i = 0; i < src.planes.size(); ++i) {
        const auto& sp = src.planes[i];
        auto& dp = buffer->planes[i];

        if (sp.dma_fd < 0) {
            RKCAM_LOGE("wrapSourceFrameAsDmaFrame failed: plane[%zu] invalid dma_fd=%d",
                       i,
                       sp.dma_fd);
            return false;
        }

        if (sp.length == 0 || sp.stride <= 0) {
            RKCAM_LOGE("wrapSourceFrameAsDmaFrame failed: plane[%zu] invalid length=%zu stride=%d",
                       i,
                       sp.length,
                       sp.stride);
            return false;
        }

        /*
         * V4L2 MMAP + EXPBUF:
         *   data 可以保留，方便调试。
         *   RGA 实际主要用 dma_fd。
         */
        dp.data = static_cast<uint8_t*>(sp.data);
        dp.cpu_storage.clear();

        dp.dma_fd = sp.dma_fd;
        dp.offset = 0;

        dp.length = sp.length;
        dp.bytesused = sp.bytesused;
        dp.stride = sp.stride;

        /*
         * 当前 V4L2 输入是 NV12 single-plane。
         * source frame 里如果没有 height_stride 字段，
         * 这里先用 visible height。
         *
         * 对 RKISP 输出 1920x1080 NV12，通常这样可以。
         */
        dp.height_stride = src.height;
    }

    dst.buffer = std::move(buffer);
    return true;
}

bool prepareRgaOutputFrame(
    rkcam::MppBufferPool& pool,
    const rkcam::PipelineVideoFrame& input,
    rkcam::PipelineVideoFrame& output)
{
    auto out_buffer = pool.acquire();
    if (!out_buffer) {
        RKCAM_LOGE("prepareRgaOutputFrame failed: acquire MPP buffer failed");
        return false;
    }

    output = rkcam::PipelineVideoFrame{};

    output.stream_id = input.stream_id;
    output.width = kOutputWidth;
    output.height = kOutputHeight;
    output.format = rkcam::PixelFormat::NV12;
    output.pts_us = input.pts_us;
    output.duration_us = input.duration_us;
    output.frame_id = input.frame_id;
    output.buffer = std::move(out_buffer);

    return true;
}

} // namespace

int main()
{
    RKCAM_LOGI("mpp_encoder_test start");

    FILE* fp = std::fopen(kOutputPath, "wb");
    if (!fp) {
        RKCAM_LOGE("failed to open output file: %s", kOutputPath);
        return 1;
    }

    /*
     * 1. V4L2 source
     */
    rkcam::V4L2VideoSourceConfig source_config;
    source_config.device = kDevice;
    source_config.width = kInputWidth;
    source_config.height = kInputHeight;
    source_config.pixel_format = "NV12";
    source_config.buffer_count = 4;
    source_config.export_dma_fd = true;

    rkcam::V4L2VideoSource source(source_config);

    if (!source.open()) {
        RKCAM_LOGE("source.open failed");
        std::fclose(fp);
        return 1;
    }

    if (!source.start()) {
        RKCAM_LOGE("source.start failed");
        source.close();
        std::fclose(fp);
        return 1;
    }

    /*
     * 2. RGA processor
     */
    rkcam::RgaProcessor rga;

    if (!rga.isAvailable()) {
        RKCAM_LOGE("RGA is not available");
        source.stop();
        source.close();
        std::fclose(fp);
        return 1;
    }

    rkcam::RgaProcessRequest rga_req;
    rga_req.output_width = kOutputWidth;
    rga_req.output_height = kOutputHeight;
    rga_req.output_format = rkcam::PixelFormat::NV12;
    rga_req.output_memory_type = rkcam::VideoMemoryType::DmaBuffer;
    rga_req.allow_fallback = false;

    /*
     * 3. MPP buffer pool
     *
     * 注意：
     *   MPP buffer 是 RGA 输出，也是 Encoder 输入。
     */
    rkcam::MppBufferPoolConfig pool_config;
    pool_config.pool_name = "mpp_encoder_test_pool";
    pool_config.width = kOutputWidth;
    pool_config.height = kOutputHeight;
    pool_config.format = rkcam::PixelFormat::NV12;
    pool_config.buffer_count = 4;

    /*
     * 如果你的 MppBufferPoolConfig 有 hor_stride / ver_stride 字段，
     * 建议显式设置。
     *
     * 640x360 NV12:
     *   hor_stride = 640
     *   ver_stride = align16(360) = 368
     */
    pool_config.hor_stride = kOutputWidth;
    pool_config.ver_stride = alignTo(kOutputHeight, 16);

    auto mpp_pool = std::make_shared<rkcam::MppBufferPool>(pool_config);

    if (!mpp_pool->init()) {
        RKCAM_LOGE("mpp_pool init failed");
        source.stop();
        source.close();
        std::fclose(fp);
        return 1;
    }

    /*
     * 4. MPP encoder
     */
    rkcam::MppEncoderConfig encoder_config;
    encoder_config.width = kOutputWidth;
    encoder_config.height = kOutputHeight;
    encoder_config.input_format = rkcam::PixelFormat::NV12;

    /*
     * 设 0，让 MppEncoder 从 frame.buffer->planes[0] 推导：
     *   hor_stride = plane.stride
     *   ver_stride = plane.height_stride
     */
    encoder_config.hor_stride = 0;
    encoder_config.ver_stride = 0;

    encoder_config.codec = rkcam::CodecType::H264;
    encoder_config.fps = 30;
    encoder_config.gop = 30;
    encoder_config.bitrate = 2 * 1000 * 1000;

    rkcam::MppEncoder encoder(encoder_config);

    if (!encoder.init()) {
        RKCAM_LOGE("encoder init failed");
        mpp_pool->close();
        source.stop();
        source.close();
        std::fclose(fp);
        return 1;
    }

    int encoded_frames = 0;
    int failed_frames = 0;

    for (int i = 0; i < kMaxFrames; ++i) {
        rkcam::VideoSourceFrame src_frame;

        if (!source.readFrame(src_frame)) {
            RKCAM_LOGE("source.readFrame failed, i=%d", i);
            ++failed_frames;
            continue;
        }

        rkcam::PipelineVideoFrame input_frame;

        if (!wrapSourceFrameAsDmaFrame(src_frame, input_frame, "cam0")) {
            RKCAM_LOGE("wrapSourceFrameAsDmaFrame failed, frame_id=%lld",
                       static_cast<long long>(src_frame.frame_id));

            source.releaseFrame(src_frame);
            ++failed_frames;
            continue;
        }

        rkcam::PipelineVideoFrame rga_output;

        if (!prepareRgaOutputFrame(*mpp_pool, input_frame, rga_output)) {
            RKCAM_LOGE("prepareRgaOutputFrame failed, frame_id=%lld",
                       static_cast<long long>(src_frame.frame_id));

            source.releaseFrame(src_frame);
            ++failed_frames;
            continue;
        }

        /*
         * 当前 RgaProcessor 使用 IM_SYNC。
         * process 返回后，RGA 已经读完 input dma_fd。
         */
        if (!rga.process(input_frame, rga_output, rga_req)) {
            RKCAM_LOGE("RGA process failed, frame_id=%lld",
                       static_cast<long long>(src_frame.frame_id));

            source.releaseFrame(src_frame);
            ++failed_frames;
            continue;
        }

        /*
         * RGA 同步完成后，可以归还 V4L2 buffer。
         * 后续 encoder 只使用 rga_output 里的 MPP buffer。
         */
        if (!source.releaseFrame(src_frame)) {
            RKCAM_LOGE("source.releaseFrame failed, frame_id=%lld",
                       static_cast<long long>(src_frame.frame_id));
            ++failed_frames;
            continue;
        }

        rkcam::EncodedPacket packet;

        if (!encoder.encode(rga_output, packet)) {
            RKCAM_LOGE("encoder.encode failed, frame_id=%lld",
                       static_cast<long long>(rga_output.frame_id));
            ++failed_frames;
            continue;
        }

        if (!packet.empty()) {
            const size_t written =
                std::fwrite(packet.data(), 1, packet.size(), fp);

            if (written != packet.size()) {
                RKCAM_LOGE("fwrite failed, written=%zu expected=%zu",
                           written,
                           packet.size());
                ++failed_frames;
                break;
            }
        }

        ++encoded_frames;

        if (encoded_frames % 30 == 0 || encoded_frames == kMaxFrames) {
            RKCAM_LOGI("encoded_frames=%d frame_id=%lld packet_size=%zu key=%d",
                       encoded_frames,
                       static_cast<long long>(packet.pts_us),
                       packet.size(),
                       packet.key_frame ? 1 : 0);
        }
    }

    encoder.close();
    mpp_pool->close();

    source.stop();
    source.close();

    std::fflush(fp);
    std::fclose(fp);

    RKCAM_LOGI("mpp_encoder_test done, encoded_frames=%d failed_frames=%d output=%s",
               encoded_frames,
               failed_frames,
               kOutputPath);

    return encoded_frames > 0 ? 0 : 1;
}