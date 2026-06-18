#include "rkcam/core/log.hpp"
#include "rkcam/video/v4l2_video_source.hpp"
#include "rkcam/video/rga_processor.hpp"
#include "rkcam/pipeline/pipeline_video_frame.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstdint>

static bool saveCpuFrame(
    const rkcam::PipelineVideoFrame& frame,
    const std::string& path)
{
    if (!frame.buffer) {
        RKCAM_LOGE("saveCpuFrame failed: frame.buffer is null");
        return false;
    }

    if (frame.buffer->memory_type != rkcam::VideoMemoryType::Cpu) {
        RKCAM_LOGE("saveCpuFrame failed: frame is not Cpu memory");
        return false;
    }

    if (frame.buffer->planes.empty()) {
        RKCAM_LOGE("saveCpuFrame failed: no planes");
        return false;
    }

    const auto& plane = frame.buffer->planes[0];

    if (!plane.data || plane.bytesused == 0) {
        RKCAM_LOGE("saveCpuFrame failed: invalid plane data=%p bytesused=%zu",
                   plane.data,
                   plane.bytesused);
        return false;
    }

    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) {
        RKCAM_LOGE("saveCpuFrame failed: open %s failed", path.c_str());
        return false;
    }

    const size_t written = std::fwrite(plane.data, 1, plane.bytesused, fp);
    std::fclose(fp);

    if (written != plane.bytesused) {
        RKCAM_LOGE("saveCpuFrame failed: written=%zu expected=%zu",
                   written,
                   plane.bytesused);
        return false;
    }

    RKCAM_LOGI("saved frame: %s, width=%d height=%d format=%d bytes=%zu stride=%d",
               path.c_str(),
               frame.width,
               frame.height,
               static_cast<int>(frame.format),
               plane.bytesused,
               plane.stride);

    return true;
}

static bool runOneTest(
    rkcam::RgaProcessor& rga,
    const rkcam::PipelineVideoFrame& input,
    const rkcam::RgaProcessRequest& req,
    const std::string& output_path,
    const char* test_name)
{
    RKCAM_LOGI("========== test: %s ==========", test_name);

    rkcam::PipelineVideoFrame output;

    if (!rga.process(input, output, req)) {
        RKCAM_LOGE("RGA test failed: %s", test_name);
        return false;
    }

    if (!saveCpuFrame(output, output_path)) {
        RKCAM_LOGE("save output failed: %s", test_name);
        return false;
    }

    RKCAM_LOGI("RGA test success: %s", test_name);
    return true;
}
static bool wrapVideoSourceFrameToDmaPipelineFrame(
    const rkcam::VideoSourceFrame& src,
    rkcam::PipelineVideoFrame& dst,
    const std::string& stream_id)
{
    if (src.width <= 0 || src.height <= 0) {
        RKCAM_LOGE("wrap dma frame failed: invalid size %dx%d",
                   src.width,
                   src.height);
        return false;
    }

    if (src.format == rkcam::PixelFormat::Unknown) {
        RKCAM_LOGE("wrap dma frame failed: unknown pixel format");
        return false;
    }

    if (src.planes.empty()) {
        RKCAM_LOGE("wrap dma frame failed: src.planes is empty");
        return false;
    }

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
            RKCAM_LOGE("wrap dma frame failed: plane[%zu] invalid dma_fd=%d",
                       i,
                       sp.dma_fd);
            return false;
        }
        if (sp.length == 0) {
            RKCAM_LOGE("wrap dma frame failed: plane[%zu] length is 0",
                       i);
            return false;
        }
        if (sp.bytesused == 0) {
            RKCAM_LOGE("wrap dma frame failed: plane[%zu] bytesused is 0",
                       i);
            return false;
        }
        if (sp.stride <= 0) {
            RKCAM_LOGE("wrap dma frame failed: plane[%zu] invalid stride=%d",
                       i,
                       sp.stride);
            return false;
        }

        /*
         * DMA 模式：
         *   data 可以为空，RGA 使用 dma_fd。
         *   cpu_storage 不保存数据。
         */
        dp.data = nullptr;
        dp.cpu_storage.clear();

        /*
         * 这里只是借用 fd。
         * fd 的真正生命周期仍然由 V4L2VideoSource 管理。
         * 不要在这里 dup，也不要 close。
         */
        dp.dma_fd = sp.dma_fd;
        dp.offset = 0;

        dp.length = sp.length;
        dp.bytesused = sp.bytesused;
        dp.stride = sp.stride;
    }

    /*
     * 测试阶段不要设置 release_cb。
     * 因为当前测试程序会手动调用：
     *
     *     source.releaseFrame(src_frame);
     *
     * 后面真正做 CaptureStage::DmaBuffer 模式时，
     * 再用 release_cb 自动归还 V4L2 buffer。
     */
    buffer->release_cb = nullptr;

    dst.buffer = buffer;

    return true;
}
static bool saveVideoSourceFrameNv12Tight(
    const rkcam::VideoSourceFrame& frame,
    const std::string& path)
{
    if (frame.format != rkcam::PixelFormat::NV12) {
        RKCAM_LOGE("saveVideoSourceFrameNv12Tight only supports NV12 now, format=%d",
                   static_cast<int>(frame.format));
        return false;
    }

    if (frame.width <= 0 || frame.height <= 0) {
        RKCAM_LOGE("saveVideoSourceFrameNv12Tight invalid size: %dx%d",
                   frame.width,
                   frame.height);
        return false;
    }

    if (frame.planes.empty()) {
        RKCAM_LOGE("saveVideoSourceFrameNv12Tight failed: no planes");
        return false;
    }

    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) {
        RKCAM_LOGE("saveVideoSourceFrameNv12Tight failed: open %s failed",
                   path.c_str());
        return false;
    }

    const int width = frame.width;
    const int height = frame.height;

    bool ok = true;

    /*
     * 当前你的 /dev/video0 + NV12 日志是：
     *   planes = 1
     *   plane[0] = Y + UV 连续
     */
    if (frame.planes.size() == 1) {
        const auto& p0 = frame.planes[0];

        if (!p0.data || p0.stride <= 0) {
            RKCAM_LOGE("save original NV12 failed: invalid plane0 data=%p stride=%d",
                       p0.data,
                       p0.stride);
            std::fclose(fp);
            return false;
        }

        const uint8_t* base = static_cast<const uint8_t*>(p0.data);
        const int stride = p0.stride;

        const size_t required =
            static_cast<size_t>(stride) *
            static_cast<size_t>(height) *
            3 / 2;

        if (p0.length < required && p0.bytesused < required) {
            RKCAM_LOGE("save original NV12 failed: buffer too small, length=%zu bytesused=%zu required=%zu",
                       p0.length,
                       p0.bytesused,
                       required);
            std::fclose(fp);
            return false;
        }

        /*
         * 保存 tight NV12：
         *   每行只写 width 字节，不写 stride padding。
         */
        for (int y = 0; y < height; ++y) {
            const uint8_t* row = base + static_cast<size_t>(y) * stride;
            if (std::fwrite(row, 1, width, fp) != static_cast<size_t>(width)) {
                ok = false;
                break;
            }
        }

        const uint8_t* uv_base =
            base + static_cast<size_t>(stride) * static_cast<size_t>(height);

        for (int y = 0; ok && y < height / 2; ++y) {
            const uint8_t* row = uv_base + static_cast<size_t>(y) * stride;
            if (std::fwrite(row, 1, width, fp) != static_cast<size_t>(width)) {
                ok = false;
                break;
            }
        }
    }
    /*
     * 预留：如果后面是 NV12M / NM12，可能是两 plane：
     *   plane[0] = Y
     *   plane[1] = UV
     */
    else if (frame.planes.size() >= 2) {
        const auto& y_plane = frame.planes[0];
        const auto& uv_plane = frame.planes[1];

        if (!y_plane.data || !uv_plane.data ||
            y_plane.stride <= 0 || uv_plane.stride <= 0) {
            RKCAM_LOGE("save original NV12 mplane failed: invalid planes");
            std::fclose(fp);
            return false;
        }

        const uint8_t* y_base = static_cast<const uint8_t*>(y_plane.data);
        const uint8_t* uv_base = static_cast<const uint8_t*>(uv_plane.data);

        for (int y = 0; y < height; ++y) {
            const uint8_t* row =
                y_base + static_cast<size_t>(y) * y_plane.stride;

            if (std::fwrite(row, 1, width, fp) != static_cast<size_t>(width)) {
                ok = false;
                break;
            }
        }

        for (int y = 0; ok && y < height / 2; ++y) {
            const uint8_t* row =
                uv_base + static_cast<size_t>(y) * uv_plane.stride;

            if (std::fwrite(row, 1, width, fp) != static_cast<size_t>(width)) {
                ok = false;
                break;
            }
        }
    } else {
        RKCAM_LOGE("save original NV12 failed: unsupported planes=%zu",
                   frame.planes.size());
        ok = false;
    }

    std::fclose(fp);

    if (!ok) {
        RKCAM_LOGE("save original NV12 failed while writing: %s",
                   path.c_str());
        return false;
    }

    RKCAM_LOGI("saved original NV12 frame: %s, width=%d height=%d planes=%zu",
               path.c_str(),
               frame.width,
               frame.height,
               frame.planes.size());

    return true;
}
int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    RKCAM_LOGI("rga_process_test start");

    rkcam::V4L2VideoSourceConfig source_cfg;
    source_cfg.device = "/dev/video0";
    source_cfg.width = 1920;
    source_cfg.height = 1080;
    source_cfg.pixel_format = "NV12";
    source_cfg.buffer_count = 4;
    source_cfg.export_dma_fd = true;

    rkcam::V4L2VideoSource source(source_cfg);

    if (!source.open()) {
        RKCAM_LOGE("source.open failed");
        return 1;
    }

    if (!source.start()) {
        RKCAM_LOGE("source.start failed");
        source.close();
        return 1;
    }

    rkcam::VideoSourceFrame src_frame;

    if (!source.readFrame(src_frame)) {
        RKCAM_LOGE("source.readFrame failed");
        source.stop();
        source.close();
        return 1;
    }
    if (!saveVideoSourceFrameNv12Tight(
            src_frame,
            "/userdata/rkcam/output/original_1920x1080_nv12.yuv")) {
        RKCAM_LOGE("save original frame failed");
        source.releaseFrame(src_frame);
        source.stop();
        source.close();
        return 1;
    }
    RKCAM_LOGI("capture frame: width=%d height=%d format=%d planes=%zu buffer_index=%d",
               src_frame.width,
               src_frame.height,
               static_cast<int>(src_frame.format),
               src_frame.planes.size(),
               src_frame.buffer_index);

    for (size_t i = 0; i < src_frame.planes.size(); ++i) {
        const auto& p = src_frame.planes[i];
        RKCAM_LOGI("  src plane[%zu]: fd=%d stride=%d length=%zu bytesused=%zu data=%p",
                   i,
                   p.dma_fd,
                   p.stride,
                   p.length,
                   p.bytesused,
                   p.data);
    }

    rkcam::PipelineVideoFrame input;

    if (!wrapVideoSourceFrameToDmaPipelineFrame(
            src_frame,
            input,
            "cam0")) {
        RKCAM_LOGE("wrapVideoSourceFrameToDmaPipelineFrame failed");
        source.releaseFrame(src_frame);
        source.stop();
        source.close();
        return 1;
    }

    rkcam::RgaProcessor rga;

    if (!rga.isAvailable()) {
        RKCAM_LOGE("RGA is not available");
        source.releaseFrame(src_frame);
        source.stop();
        source.close();
        return 1;
    }

    bool ok = true;

    /*
     * Test 1: resize only
     * NV12 1920x1080 DMA -> NV12 640x360 CPU
     */
    {
        rkcam::RgaProcessRequest req;
        req.output_width = 640;
        req.output_height = 360;
        req.output_format = rkcam::PixelFormat::NV12;
        req.output_memory_type = rkcam::VideoMemoryType::Cpu;
        req.allow_fallback = false;

        ok &= runOneTest(
            rga,
            input,
            req,
            "/userdata/rkcam/output/rga_resize_640x360_nv12.yuv",
            "resize NV12->NV12 640x360");
    }

        /*
     * Test 2: convert only
     * NV12 1920x1080 DMA -> RGB888 1920x1080 CPU
     *
     * 如果你 PixelFormat 还没加 RGB888，先注释这个测试。
     */
    {
        rkcam::RgaProcessRequest req;
        req.output_width = input.width;
        req.output_height = input.height;
        req.output_format = rkcam::PixelFormat::RGB888;
        req.output_memory_type = rkcam::VideoMemoryType::Cpu;
        req.allow_fallback = false;

        ok &= runOneTest(
            rga,
            input,
            req,
            "/userdata/rkcam/output/rga_convert_1920x1080_rgb888.rgb",
            "convert NV12->RGB888 1920x1080");
    }

    /*
     * Test 3: resize + convert
     * NV12 1920x1080 DMA -> RGB888 640x360 CPU
     *
     * 这是验证 improcess 单次复合处理最关键的测试。
     */
    {
        rkcam::RgaProcessRequest req;
        req.output_width = 640;
        req.output_height = 360;
        req.output_format = rkcam::PixelFormat::RGB888;
        req.output_memory_type = rkcam::VideoMemoryType::Cpu;
        req.allow_fallback = false;

        ok &= runOneTest(
            rga,
            input,
            req,
            "/userdata/rkcam/output/rga_resize_convert_640x360_rgb888.rgb",
            "resize+convert NV12->RGB888 640x360");
    }

    /*
     * Test 4: crop + resize
     * 从中间裁 960x540，再缩放到 640x360。
     * 注意 NV12/YUV420 的 crop x/y/w/h 都要偶数。
     */
    {
        rkcam::RgaProcessRequest req;
        req.src_rect.x = 480;
        req.src_rect.y = 270;
        req.src_rect.width = 960;
        req.src_rect.height = 540;
        req.output_width = 640;
        req.output_height = 360;
        req.output_format = rkcam::PixelFormat::NV12;
        req.output_memory_type = rkcam::VideoMemoryType::Cpu;
        req.allow_fallback = false;

        ok &= runOneTest(
            rga,
            input,
            req,
            "/userdata/rkcam/output/rga_crop_resize_640x360_nv12.yuv",
            "crop+resize NV12->NV12 640x360");
    }

    /*
     * Test 5: horizontal flip
     */
    {
        rkcam::RgaProcessRequest req;
        req.output_width = 640;
        req.output_height = 360;
        req.output_format = rkcam::PixelFormat::NV12;
        req.output_memory_type = rkcam::VideoMemoryType::Cpu;
        req.flip = rkcam::RgaFlipMode::Horizontal;
        req.allow_fallback = false;

        ok &= runOneTest(
            rga,
            input,
            req,
            "/userdata/rkcam/output/rga_flip_h_640x360_nv12.yuv",
            "resize+horizontal flip NV12->NV12 640x360");
    }

    /*
    * Test 6: resize + rotate 90
    * NV12 1920x1080 DMA -> NV12 360x640 CPU
    *
    * 注意：
    *   原始方向 1920x1080
    *   先缩放到类似 640x360，再旋转 90 度后，输出应该是 360x640。
    */
    {
        rkcam::RgaProcessRequest req;
        req.output_width = 360;
        req.output_height = 640;
        req.output_format = rkcam::PixelFormat::NV12;
        req.output_memory_type = rkcam::VideoMemoryType::Cpu;
        req.rotate = rkcam::RgaRotateMode::Rotate90;
        req.allow_fallback = false;

        ok &= runOneTest(
            rga,
            input,
            req,
            "/userdata/rkcam/output/rga_rotate90_360x640_nv12.yuv",
            "resize+rotate90 NV12->NV12 360x640");
    }
    

    source.releaseFrame(src_frame);
    source.stop();
    source.close();

    RKCAM_LOGI("rga_process_test done, ok=%d", ok ? 1 : 0);
    return ok ? 0 : 1;

}