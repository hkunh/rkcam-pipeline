#pragma once

#include "rkcam/video/video_frame_processor.hpp"

#include <cstddef>
#include <cstdint>

namespace rkcam {

struct RgaRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

enum class RgaFlipMode {
    None,
    Horizontal,
    Vertical,
    Both,
};

enum class RgaRotateMode {
    None,
    Rotate90,
    Rotate180,
    Rotate270,
};

struct RgaProcessRequest {
    /*
     * 输入裁剪区域。
     * width/height <= 0 表示整张图。
     */
    RgaRect src_rect;

    /*
     * 输出尺寸。
     * <= 0 表示根据 src_rect 或输入尺寸自动决定。
     */
    int output_width = 0;
    int output_height = 0;

    /*
     * PixelFormat::Unknown 表示输出格式保持 input.format。
     */
    PixelFormat output_format = PixelFormat::Unknown;

    /*
     * 当前第一版只实现 Cpu 输出。
     * 后面接 MPP 时再实现 DmaBuffer 输出。
     */
    VideoMemoryType output_memory_type = VideoMemoryType::Cpu;

    RgaFlipMode flip = RgaFlipMode::None;
    RgaRotateMode rotate = RgaRotateMode::None;

    /*
     * 当前 fallback 先预留。
     * 现在 processFallbackDmaToCpu() 会直接返回 false。
     */
    bool allow_fallback = false;
};

class RgaProcessor : public IVideoFrameProcessor {
public:
    RgaProcessor() = default;
    explicit RgaProcessor(const RgaProcessRequest& default_request);

    ~RgaProcessor() override = default;

    RgaProcessor(const RgaProcessor&) = delete;
    RgaProcessor& operator=(const RgaProcessor&) = delete;

    bool isAvailable() const;

    void setDefaultRequest(const RgaProcessRequest& request);

    /*
     * 兼容 IVideoFrameProcessor。
     * 使用 default_request_。
     */
    bool process(
        const PipelineVideoFrame& input,
        PipelineVideoFrame& output) override;

    /*
     * 推荐主接口。
     * 上层描述“我要什么结果”，RgaProcessor 内部决定怎么调用 RGA。
     */
    bool process(
        const PipelineVideoFrame& input,
        PipelineVideoFrame& output,
        const RgaProcessRequest& request);

    /*
     * 便捷接口：内部都调用 process(input, output, request)。
     */
    bool resize(
        const PipelineVideoFrame& input,
        PipelineVideoFrame& output,
        int output_width,
        int output_height,
        PixelFormat output_format = PixelFormat::Unknown,
        VideoMemoryType output_memory_type = VideoMemoryType::Cpu);

    bool convert(
        const PipelineVideoFrame& input,
        PipelineVideoFrame& output,
        PixelFormat output_format,
        VideoMemoryType output_memory_type = VideoMemoryType::Cpu);

    bool flip(
        const PipelineVideoFrame& input,
        PipelineVideoFrame& output,
        RgaFlipMode mode,
        VideoMemoryType output_memory_type = VideoMemoryType::Cpu);

    bool rotate(
        const PipelineVideoFrame& input,
        PipelineVideoFrame& output,
        RgaRotateMode mode,
        VideoMemoryType output_memory_type = VideoMemoryType::Cpu);

private:
    bool processDmaToCpu(
        const PipelineVideoFrame& input,
        PipelineVideoFrame& output,
        const RgaProcessRequest& request);

    /*
     * 一次 RGA 复合操作。
     * 当前核心路径：
     *   DmaBuffer single-plane → Cpu
     */
    bool processSinglePassDmaToCpu(
        const PipelineVideoFrame& input,
        PipelineVideoFrame& output,
        const RgaProcessRequest& request);

    /*
     * 后面再做：
     *   resize + convert 两步 fallback
     *   crop + resize fallback
     */
    bool processFallbackDmaToCpu(
        const PipelineVideoFrame& input,
        PipelineVideoFrame& output,
        const RgaProcessRequest& request);

private:
    bool validateDmaInput(const PipelineVideoFrame& input) const;
    bool validateSinglePlaneInput(const PipelineVideoFrame& input) const;

    bool makeSourceRect(
        const PipelineVideoFrame& input,
        const RgaProcessRequest& request,
        RgaRect& rect) const;

    bool createCpuOutputFrame(
        int width,
        int height,
        PixelFormat format,
        PipelineVideoFrame& output) const;

private:
    static bool isYuv420Format(PixelFormat format);
    static bool isRgbFormat(PixelFormat format);

    static int alignEven(int value);

    /*
     * RGA 的 wstride 是像素 stride。
     * 你的 VideoBufferPlane::stride 建议继续表示 bytes per line。
     */
    static int calcRgaWStride(PixelFormat format, int width);
    static int calcPlaneStrideBytes(PixelFormat format, int width);
    static size_t calcPixelFormatSize(PixelFormat format, int stride_bytes, int height);

    static int pixelFormatToRgaFormat(PixelFormat format);

    static int buildTransformUsage(const RgaProcessRequest& request);

private:
    RgaProcessRequest default_request_;
};

} // namespace rkcam