#include "rkcam/platform/rockchip/rga/rga_processor.hpp"
#include "rkcam/core/log.hpp"
#include "rkcam/platform/rockchip/rga/rga_rk3568/rga.h"
#include "rkcam/platform/rockchip/rga/rga_rk3568/im2d.h"
#include "rkcam/platform/rockchip/rga/rga_rk3568/RgaUtils.h"

#include <cstring>
#include <memory>

namespace rkcam {

RgaProcessor::RgaProcessor(const RgaProcessRequest& default_request)
    : default_request_(default_request)
{
}

bool RgaProcessor::isAvailable() const
{
    const char* version = querystring(RGA_VERSION);
    return version != nullptr;
}

void RgaProcessor::setDefaultRequest(const RgaProcessRequest& request)
{
    default_request_ = request;
}

bool RgaProcessor::process(
    const PipelineVideoFrame& input,
    PipelineVideoFrame& output)
{
    return process(input, output, default_request_);
}

bool RgaProcessor::process(
    const PipelineVideoFrame& input,
    PipelineVideoFrame& output,
    const RgaProcessRequest& request)
{
    if (!input.buffer) {
        RKCAM_LOGE("RgaProcessor process failed: input.buffer is null");
        return false;
    }

    if (input.buffer->memory_type == VideoMemoryType::DmaBuffer &&
        request.output_memory_type == VideoMemoryType::Cpu) {
        return processDmaToCpu(input, output, request);
    }
    if (input.buffer->memory_type == VideoMemoryType::DmaBuffer &&
        request.output_memory_type == VideoMemoryType::DmaBuffer) {
        /*
         * DMA 输出必须由 RgaStage 提前准备 output.buffer。
         */
        if (!output.buffer ||
            output.buffer->memory_type != VideoMemoryType::DmaBuffer) {
            RKCAM_LOGE("RgaProcessor Dma->Dma requires preallocated output DmaBuffer");
            return false;
        }

        return processDmaToDma(input, output, request);
    }

    RKCAM_LOGE("RgaProcessor unsupported memory path: input_memory=%d output_memory=%d",
               static_cast<int>(input.buffer->memory_type),
               static_cast<int>(request.output_memory_type));

    return false;
}

bool RgaProcessor::resize(
    const PipelineVideoFrame& input,
    PipelineVideoFrame& output,
    int output_width,
    int output_height,
    PixelFormat output_format,
    VideoMemoryType output_memory_type)
{
    RgaProcessRequest request;
    request.output_width = output_width;
    request.output_height = output_height;
    request.output_format = output_format == PixelFormat::Unknown
        ? input.format
        : output_format;
    request.output_memory_type = output_memory_type;
    request.allow_fallback = false;

    return process(input, output, request);
}
bool RgaProcessor::processDmaToCpu(
    const PipelineVideoFrame& input,
    PipelineVideoFrame& output,
    const RgaProcessRequest& request)
{
    if (!validateDmaInput(input)) {
        return false;
    }

    /*
     * 你的 im2d.h 当前 wrapbuffer_fd() 只能接一个 fd。
     * 所以当前只支持 single-plane DMA input。
     */
    if (!validateSinglePlaneInput(input)) {
        return false;
    }

    if (processSinglePassDmaToCpu(input, output, request)) {
        return true;
    }

    if (!request.allow_fallback) {
        RKCAM_LOGE("RgaProcessor single-pass failed and fallback disabled");
        return false;
    }

    return processFallbackDmaToCpu(input, output, request);
}
bool RgaProcessor::processSinglePassDmaToCpu(
    const PipelineVideoFrame& input,
    PipelineVideoFrame& output,
    const RgaProcessRequest& request)
{
    const auto& src_plane = input.buffer->planes[0];
    const int src_rga_format = pixelFormatToRgaFormat(input.format);
    if (src_rga_format < 0) {
        RKCAM_LOGE("RgaProcessor unsupported input format=%d",
                   static_cast<int>(input.format));
        return false;
    }
    const int src_rga_wstride = calcRgaWStrideFromPlaneStride(input.format, src_plane.stride);
    const int src_rga_hstride = src_plane.height_stride > 0 ? src_plane.height_stride : input.height;
    if (src_rga_wstride <= 0) {
        RKCAM_LOGE("RgaProcessor invalid rga stride: src=%d",
                   src_rga_wstride);
        return false;
    }

    PixelFormat dst_format = request.output_format;
    const int dst_rga_format = pixelFormatToRgaFormat(dst_format);
    if (dst_rga_format < 0) {
        RKCAM_LOGE("RgaProcessor unsupported output format=%d",
                   static_cast<int>(dst_format));
        return false;
    }
    RgaRect src_rect_rk {};
    if(!makeSourceRect(input, request, src_rect_rk)){
        return false;
    }
    int dst_width = request.output_width > 0 ? request.output_width : src_rect_rk.width;
    int dst_height = request.output_height > 0 ?request.output_height : src_rect_rk.height;
    if((request.rotate == RgaRotateMode::Rotate90 || request.rotate == RgaRotateMode::Rotate270) && request.output_width <= 0 && request.output_height <=0)
    {
        dst_width = src_rect_rk.height;
        dst_height = src_rect_rk.width;
    }
    if(isYuv420Format(dst_format))
    {
        dst_width = alignEven(dst_width);
        dst_height = alignEven(dst_height);
    }
    if (dst_width <= 0 || dst_height <= 0) {
        RKCAM_LOGE("RgaProcessor invalid output size: %dx%d",
                   dst_width,
                   dst_height);
        return false;
    }
    if (!createCpuOutputFrame(dst_width, dst_height, dst_format, output)) {
        return false;
    }
    auto& dst_plane = output.buffer->planes[0];
    const int dst_rga_wstride = calcRgaWStride(dst_format, dst_width);
    const int dst_rga_hstride = dst_height;
    if (src_rga_wstride <= 0 || dst_rga_wstride <= 0) {
        RKCAM_LOGE("RgaProcessor invalid rga stride: dst=%d",
                   dst_rga_wstride);
        return false;
    }


    rga_buffer_t src_img = wrapbuffer_fd(
        src_plane.dma_fd,
        input.width,
        input.height,
        src_rga_format,
        src_rga_wstride,
        src_rga_hstride);
    rga_buffer_t dst_img = wrapbuffer_virtualaddr(
        dst_plane.data,
        dst_width,
        dst_height,
        dst_rga_format,
        dst_rga_wstride,
        dst_rga_hstride);

    if(isYuv420Format(input.format) && isRgbFormat(dst_format))
    {
        dst_img.color_space_mode = IM_YUV_TO_RGB_BT601_LIMIT;
    }
    else if(isRgbFormat(input.format) && isYuv420Format(dst_format)){
        dst_img.color_space_mode = IM_RGB_TO_YUV_BT601_LIMIT;
    }
    else{
        src_img.color_space_mode = IM_COLOR_SPACE_DEFAULT;
        dst_img.color_space_mode = IM_COLOR_SPACE_DEFAULT;
    }

    im_rect src_rect{};
    src_rect.x = src_rect_rk.x;
    src_rect.y = src_rect_rk.y;
    src_rect.width = src_rect_rk.width;
    src_rect.height = src_rect_rk.height;

    im_rect dst_rect {};
    dst_rect.x = 0;
    dst_rect.y = 0;
    dst_rect.width = dst_width;
    dst_rect.height = dst_height;

    rga_buffer_t pat {};
    std::memset(&pat, 0, sizeof(pat));
    im_rect pat_rect {};
    std::memset(&pat_rect, 0, sizeof(pat_rect));

    int usage = buildTransformUsage(request);
    usage |= IM_SYNC;

    /*
     * 核心：一次 RGA 复合处理。
     * src/dst format 不同 => convert
     * src_rect/dst_rect 尺寸不同 => resize
     * src_rect 非整图 => crop
     * usage 中有 transform => flip/rotate
     */
    IM_STATUS status = improcess(
        src_img,
        dst_img,
        pat,
        src_rect,
        dst_rect,
        pat_rect,
        usage);

    if (status != IM_STATUS_SUCCESS) {
        RKCAM_LOGE("RGA improcess failed: %s, usage=0x%x",
                   imStrError(status),
                   usage);
        return false;
    }

    output.stream_id = input.stream_id;
    output.width = dst_width;
    output.height = dst_height;
    output.format = dst_format;
    output.pts_us = input.pts_us;
    output.duration_us = input.duration_us;
    output.frame_id = input.frame_id;

    return true;
}
int RgaProcessor::calcRgaWStrideFromPlaneStride(
    PixelFormat format,
    int plane_stride_bytes)
{
    if (plane_stride_bytes <= 0) {
        return 0;
    }

    switch (format) {
    case PixelFormat::NV12:
        /*
         * NV12:
         *   Y 分量 1 pixel = 1 byte
         *   所以 bytes stride == pixel stride
         */
        return plane_stride_bytes;

    case PixelFormat::RGB888:
    case PixelFormat::BGR888:
        /*
         * RGB888/BGR888:
         *   1 pixel = 3 bytes
         *   RGA wstride 传像素数
         */
        if (plane_stride_bytes % 3 != 0) {
            RKCAM_LOGE("RgaProcessor invalid RGB stride bytes=%d, not divisible by 3",
                       plane_stride_bytes);
            return 0;
        }

        return plane_stride_bytes / 3;

    default:
        return 0;
    }
}
bool RgaProcessor::processDmaToDma(
    const PipelineVideoFrame& input,
    PipelineVideoFrame& output,
    const RgaProcessRequest& request)
{
    /*
     * 1. 校验输入 DMA frame
     */
    if (!validateDmaInput(input)) {
        return false;
    }

    /*
     * 当前 RGA wrapbuffer_fd() 路径先只支持：
     *   一帧一个 dma_fd
     *
     * 例如：
     *   NV12: plane[0] = Y + UV
     */
    if (!validateSinglePlaneInput(input)) {
        return false;
    }

    /*
     * 2. 校验输出 DMA frame
     *
     * 注意：
     *   output.buffer 必须由 RgaStage 从 DmaBufferPool 提前准备。
     *   RgaProcessor 不申请 DMA buffer。
     */
    if (!output.buffer) {
        RKCAM_LOGE("RgaProcessor processDmaToDma failed: output.buffer is null");
        return false;
    }

    if (output.buffer->memory_type != VideoMemoryType::DmaBuffer) {
        RKCAM_LOGE("RgaProcessor processDmaToDma failed: output is not DmaBuffer");
        return false;
    }

    if (output.buffer->planes.size() != 1) {
        RKCAM_LOGE("RgaProcessor processDmaToDma only supports single-plane output now, planes=%zu",
                   output.buffer->planes.size());
        return false;
    }
    const auto& src_plane = input.buffer->planes[0];
    auto& dst_plane = output.buffer->planes[0];

    if (src_plane.dma_fd < 0 || dst_plane.dma_fd < 0) {
        RKCAM_LOGE("RgaProcessor processDmaToDma invalid fd: src_fd=%d dst_fd=%d",
                   src_plane.dma_fd,
                   dst_plane.dma_fd);
        return false;
    }

    if (src_plane.stride <= 0 || dst_plane.stride <= 0) {
        RKCAM_LOGE("RgaProcessor processDmaToDma invalid stride: src_stride=%d dst_stride=%d",
                   src_plane.stride,
                   dst_plane.stride);
        return false;
    }

    if (dst_plane.length == 0) {
        RKCAM_LOGE("RgaProcessor processDmaToDma dst length is 0");
        return false;
    }

    const PixelFormat src_format = input.format;
    PixelFormat dst_format = request.output_format;

    const int src_rga_format = pixelFormatToRgaFormat(src_format);
    const int dst_rga_format = pixelFormatToRgaFormat(dst_format);

    RgaRect src_rect_rk {};
    if (!makeSourceRect(input, request, src_rect_rk)) {
        return false;
    }
    int dst_width = request.output_width > 0
        ? request.output_width
        : src_rect_rk.width;

    int dst_height = request.output_height > 0
        ? request.output_height
        : src_rect_rk.height;

    /*
     * 如果旋转 90/270，且用户没显式设置输出尺寸，
     * 默认交换宽高。
     */
    if ((request.rotate == RgaRotateMode::Rotate90 ||
         request.rotate == RgaRotateMode::Rotate270) &&
        request.output_width <= 0 &&
        request.output_height <= 0) {
        dst_width = src_rect_rk.height;
        dst_height = src_rect_rk.width;
    }

    if (isYuv420Format(dst_format)) {
        dst_width = alignEven(dst_width);
        dst_height = alignEven(dst_height);
    }

    if (dst_width <= 0 || dst_height <= 0) {
        RKCAM_LOGE("RgaProcessor processDmaToDma invalid output size: %dx%d",
                   dst_width,
                   dst_height);
        return false;
    }
    

    /*
     * output frame 通常已经由 RgaStage 填好了。
     * 这里做一致性检查。
     */
    if (output.width != dst_width ||
        output.height != dst_height ||
        output.format != dst_format) {
        RKCAM_LOGE("RgaProcessor processDmaToDma output metadata mismatch: "
                   "output=%dx%d fmt=%d, expected=%dx%d fmt=%d",
                   output.width,
                   output.height,
                   static_cast<int>(output.format),
                   dst_width,
                   dst_height,
                   static_cast<int>(dst_format));
        return false;
    }
    /*
     * 6. 检查输出 buffer 大小是否足够
     *
     * dst_plane.stride 是 bytes per line。
     */
    const size_t required_dst_size = calcPixelFormatSize(dst_format, dst_plane.stride, dst_height);
    if (dst_plane.length < required_dst_size) {
        RKCAM_LOGE("RgaProcessor processDmaToDma dst buffer too small: length=%zu required=%zu",
                   dst_plane.length,
                   required_dst_size);
        return false;
    }
    dst_plane.bytesused = required_dst_size;

        /*
     * 7. 计算 RGA wstride/hstride
     *
     * 注意：
     *   VideoBufferPlane::stride 是 bytes per line。
     *   RGA wstride 通常是 pixel stride。
     */
    const int src_rga_wstride =
        calcRgaWStrideFromPlaneStride(src_format, src_plane.stride);

    const int dst_rga_wstride =
        calcRgaWStrideFromPlaneStride(dst_format, dst_plane.stride);

    const int src_rga_hstride = src_plane.height_stride > 0 ? src_plane.height_stride : input.height;
    const int dst_rga_hstride = dst_plane.height_stride > 0 ? dst_plane.height_stride : dst_height;

    if (src_rga_wstride <= 0 || dst_rga_wstride <= 0) {
        RKCAM_LOGE("RgaProcessor processDmaToDma invalid rga wstride: src=%d dst=%d",
                   src_rga_wstride,
                   dst_rga_wstride);
        return false;
    }

    /*
     * 8. 包装 RGA buffer
     */
    rga_buffer_t src_img = wrapbuffer_fd(
        src_plane.dma_fd,
        input.width,
        input.height,
        src_rga_format,
        src_rga_wstride,
        src_rga_hstride
    );

    rga_buffer_t  dst_img = wrapbuffer_fd(
        dst_plane.dma_fd,
        dst_width,
        dst_height,
        dst_rga_format,
        dst_rga_wstride,
        dst_rga_hstride
    );

    if(isYuv420Format(src_format) && isRgbFormat(dst_format))
    {
        dst_img.color_space_mode = IM_YUV_TO_RGB_BT601_LIMIT;
    }
    else if(isRgbFormat(src_format) && isYuv420Format(dst_format))
    {
        dst_img.color_space_mode = IM_RGB_TO_YUV_BT601_LIMIT;
    }
    else{
        src_img.color_space_mode = IM_COLOR_SPACE_DEFAULT;
        dst_img.color_space_mode = IM_COLOR_SPACE_DEFAULT;
    }

    /*
     * 10. RGA rect
     */
    im_rect src_rect {};
    src_rect.x = src_rect_rk.x;
    src_rect.y = src_rect_rk.y;
    src_rect.width = src_rect_rk.width;
    src_rect.height = src_rect_rk.height;

    im_rect dst_rect {};
    dst_rect.x = 0;
    dst_rect.y = 0;
    dst_rect.width = dst_width;
    dst_rect.height = dst_height;

    rga_buffer_t pat {};
    std::memset(&pat, 0, sizeof(pat));

    im_rect pat_rect {};
    std::memset(&pat_rect, 0, sizeof(pat_rect));

    int usage = buildTransformUsage(request);
    usage |= IM_SYNC;

    /*
     * 11. 执行 RGA
     *
     * src/dst format 不同 => convert
     * src_rect/dst_rect 尺寸不同 => resize
     * src_rect 非整图 => crop
     * usage 中有 transform => flip/rotate
     */
    IM_STATUS status = improcess(
        src_img,
        dst_img,
        pat,
        src_rect,
        dst_rect,
        pat_rect,
        usage);

    if (status != IM_STATUS_SUCCESS) {
        RKCAM_LOGE("RGA DmaToDma improcess failed: %s, usage=0x%x",
                   imStrError(status),
                   usage);
        return false;
    }

    /*
     * 12. 补全输出元信息
     */
    output.stream_id = input.stream_id;
    output.width = dst_width;
    output.height = dst_height;
    output.format = dst_format;
    output.pts_us = input.pts_us;
    output.duration_us = input.duration_us;
    output.frame_id = input.frame_id;

    return true;
}
bool RgaProcessor::processFallbackDmaToCpu(
    const PipelineVideoFrame& input,
    PipelineVideoFrame& output,
    const RgaProcessRequest& request)
{
    (void)input;
    (void)output;
    (void)request;

    RKCAM_LOGE("RgaProcessor fallback path is not implemented yet");
    return false;
}

bool RgaProcessor::validateDmaInput(const PipelineVideoFrame& input) const
{
    if (!input.buffer) {
        RKCAM_LOGE("RgaProcessor validateDmaInput failed: input.buffer is null");
        return false;
    }

    if (input.buffer->memory_type != VideoMemoryType::DmaBuffer) {
        RKCAM_LOGE("RgaProcessor validateDmaInput failed: input is not DmaBuffer");
        return false;
    }

    if (input.width <= 0 || input.height <= 0) {
        RKCAM_LOGE("RgaProcessor validateDmaInput failed: invalid input size %dx%d",
                   input.width,
                   input.height);
        return false;
    }

    if (input.buffer->planes.empty()) {
        RKCAM_LOGE("RgaProcessor validateDmaInput failed: planes is empty");
        return false;
    }

    for (size_t i = 0; i < input.buffer->planes.size(); ++i) {
        const auto& plane = input.buffer->planes[i];

        if (plane.dma_fd < 0) {
            RKCAM_LOGE("RgaProcessor validateDmaInput failed: plane[%zu] invalid dma_fd=%d",
                       i,
                       plane.dma_fd);
            return false;
        }

        if (plane.length == 0) {
            RKCAM_LOGE("RgaProcessor validateDmaInput failed: plane[%zu] length is 0",
                       i);
            return false;
        }

        if (plane.stride <= 0) {
            RKCAM_LOGE("RgaProcessor validateDmaInput failed: plane[%zu] stride is invalid: %d",
                       i,
                       plane.stride);
            return false;
        }
    }

    return true;
}

bool RgaProcessor::validateSinglePlaneInput(const PipelineVideoFrame& input) const
{
    if (!input.buffer) {
        return false;
    }

    if (input.buffer->planes.size() != 1) {
        RKCAM_LOGE("RgaProcessor only supports single-plane DMA input now, planes=%zu",
                   input.buffer->planes.size());
        return false;
    }

    /*
     * 当前先支持 single-plane NV12。
     * 如果后面要支持 DMA RGB 输入，再扩展这里。
     */
    if (input.format == PixelFormat::NV12) {
        const auto& plane = input.buffer->planes[0];

        if ((input.width & 1) || (input.height & 1)) {
            RKCAM_LOGE("RgaProcessor NV12 input size must be even: %dx%d",
                       input.width,
                       input.height);
            return false;
        }

        const size_t required =
            static_cast<size_t>(plane.stride) *
            static_cast<size_t>(input.height) *
            3 / 2;

        if (plane.length < required) {
            RKCAM_LOGE("RgaProcessor NV12 input buffer too small: length=%zu required=%zu",
                       plane.length,
                       required);
            return false;
        }
    }

    return true;
}

bool RgaProcessor::makeSourceRect(
    const PipelineVideoFrame& input,
    const RgaProcessRequest& request,
    RgaRect& rect) const
{
    if(request.src_rect.width > 0 && request.src_rect.height > 0)
    {
        rect = request.src_rect;
    }
    else{
        rect.x = 0;
        rect.y = 0;
        rect.width = input.width;
        rect.height = input.height;
    }
    if (rect.x < 0 || rect.y < 0 ||
        rect.width <= 0 || rect.height <= 0 ||
        rect.x + rect.width > input.width ||
        rect.y + rect.height > input.height) {
        RKCAM_LOGE("RgaProcessor invalid src_rect: x=%d y=%d w=%d h=%d input=%dx%d",
                   rect.x,
                   rect.y,
                   rect.width,
                   rect.height,
                   input.width,
                   input.height);
        return false;
    }

    /*
     * YUV420 裁剪区域需要偶数对齐。
     */
    if (isYuv420Format(input.format)) {
        if ((rect.x & 1) || (rect.y & 1) ||
            (rect.width & 1) || (rect.height & 1)) {
            RKCAM_LOGE("RgaProcessor YUV420 src_rect must be even aligned: x=%d y=%d w=%d h=%d",
                       rect.x,
                       rect.y,
                       rect.width,
                       rect.height);
            return false;
        }
    }

    return true;

}


bool RgaProcessor::createCpuOutputFrame(
    int width,
    int height,
    PixelFormat format,
    PipelineVideoFrame& output) const
{
    if (width <= 0 || height <= 0) {
        RKCAM_LOGE("RgaProcessor createCpuOutputFrame invalid size: %dx%d",
                   width,
                   height);
        return false;
    }

    const int stride_bytes = calcPlaneStrideBytes(format, width);
    if(stride_bytes <= 0)
    {
        RKCAM_LOGE("RgaProcessor createCpuOutputFrame unsupported format=%d",
                static_cast<int>(format));
        return false;
    }

    const size_t size = calcPixelFormatSize(format, stride_bytes, height);
    if (size == 0) {
        RKCAM_LOGE("RgaProcessor createCpuOutputFrame invalid buffer size, format=%d stride=%d height=%d",
                   static_cast<int>(format),
                   stride_bytes,
                   height);
        return false;
    }

    auto buffer = std::make_shared<VideoBuffer>();
    buffer->memory_type = VideoMemoryType::Cpu;
    buffer->planes.resize(1);

    auto& plane = buffer->planes[0];
    plane.cpu_storage.resize(size);
    plane.data = plane.cpu_storage.data();
    plane.length = size;
    plane.bytesused = size;
    plane.dma_fd = -1;
    plane.offset = 0;
    plane.stride = stride_bytes;

    output.width = width;
    output.height = height;
    output.format = format;
    output.buffer = buffer;

    return true;

}
bool RgaProcessor::isYuv420Format(PixelFormat format)
{
    return format == PixelFormat::NV12;
}

bool RgaProcessor::isRgbFormat(PixelFormat format)
{
    return format == PixelFormat::RGB888 ||
           format == PixelFormat::BGR888;
}

int RgaProcessor::alignEven(int value)
{
    if (value <= 0) {
        return 0;
    }

    return (value + 1) & ~1;
}


int RgaProcessor::calcRgaWStride(PixelFormat format, int width)
{
    //给rga看，返回的是像素数
    if (width <= 0) {
        return 0;
    }

    switch (format) {
    case PixelFormat::NV12:
        return alignEven(width);

    case PixelFormat::RGB888:
    case PixelFormat::BGR888:
        /*
         * RGA 的 wstride 按像素宽度传，不是 bytes per line。
         */
        return width;

    default:
        return 0;
    }
}

int RgaProcessor::calcPlaneStrideBytes(PixelFormat format, int width)
{
    //给cpu开辟空间用，返回的是字节数
    if (width <= 0) {
        return 0;
    }

    switch (format) {
    case PixelFormat::NV12:
        return alignEven(width);

    case PixelFormat::RGB888:
    case PixelFormat::BGR888:
        return width * 3;

    default:
        return 0;
    }
}

size_t RgaProcessor::calcPixelFormatSize(
    PixelFormat format,
    int stride_bytes,
    int height)
{
    if (stride_bytes <= 0 || height <= 0) {
        return 0;
    }

    switch (format) {
    case PixelFormat::NV12:
        return static_cast<size_t>(stride_bytes) *
               static_cast<size_t>(alignEven(height)) *
               3 / 2;

    case PixelFormat::RGB888:
    case PixelFormat::BGR888:
        return static_cast<size_t>(stride_bytes) *
               static_cast<size_t>(height);

    default:
        return 0;
    }
}

int RgaProcessor::pixelFormatToRgaFormat(PixelFormat format)
{
    switch (format) {
    case PixelFormat::NV12:
        return RK_FORMAT_YCbCr_420_SP;

    case PixelFormat::RGB888:
        return RK_FORMAT_RGB_888;

    case PixelFormat::BGR888:
        return RK_FORMAT_BGR_888;

    default:
        return -1;
    }
}
int RgaProcessor::buildTransformUsage(const RgaProcessRequest& request)
{
    int usage = 0;

    switch (request.rotate) {
    case RgaRotateMode::None:
        break;

    case RgaRotateMode::Rotate90:
        usage |= IM_HAL_TRANSFORM_ROT_90;
        break;

    case RgaRotateMode::Rotate180:
        usage |= IM_HAL_TRANSFORM_ROT_180;
        break;

    case RgaRotateMode::Rotate270:
        usage |= IM_HAL_TRANSFORM_ROT_270;
        break;
    }

    switch (request.flip) {
    case RgaFlipMode::None:
        break;

    case RgaFlipMode::Horizontal:
        usage |= IM_HAL_TRANSFORM_FLIP_H;
        break;

    case RgaFlipMode::Vertical:
        usage |= IM_HAL_TRANSFORM_FLIP_V;
        break;

    case RgaFlipMode::Both:
        usage |= IM_HAL_TRANSFORM_FLIP_H_V;
        break;
    }

    return usage;
}
}