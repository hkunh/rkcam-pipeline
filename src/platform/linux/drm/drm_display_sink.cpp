#include "rkcam/platform/linux/drm/drm_display_sink.hpp"

#include "rkcam/core/log.hpp"

#include <cstdint>
#include <limits>

namespace rkcam {
DrmDisplaySink::DrmDisplaySink(
    const DrmDisplaySinkConfig& config)
    : config_(config)
{
}

DrmDisplaySink::DrmDisplaySink(
    const DrmDisplaySinkConfig& config,
    std::shared_ptr<DrmDevice> device)
    : config_(config),
      drm_device_(std::move(device))
{
}

DrmDisplaySink::~DrmDisplaySink()
{
    close();
}


bool DrmDisplaySink::open()
{
    if (opened_) {
        return true;
    }

    if (config_.width <= 0 || config_.height <= 0) {
        RKCAM_LOGE("[%s] invalid display input size: %dx%d",
                   config_.sink_name.c_str(),
                   config_.width,
                   config_.height);
        return false;
    }
    if (config_.format != PixelFormat::NV12) {
        RKCAM_LOGE("[%s] only NV12 is supported now, format=%d",
                   config_.sink_name.c_str(),
                   static_cast<int>(config_.format));
        return false;
    }

    width_ = config_.width;
    height_ = config_.height;
    format_ = config_.format;

    dst_rect_ = config_.dst_rect;

    if (dst_rect_.width <= 0) {
        dst_rect_.width = width_;
    }

    if (dst_rect_.height <= 0) {
        dst_rect_.height = height_;
    }

    if (dst_rect_.width <= 0 || dst_rect_.height <= 0) {
        RKCAM_LOGE("[%s] invalid dst rect: (%d,%d,%d,%d)",
                   config_.sink_name.c_str(),
                   dst_rect_.x,
                   dst_rect_.y,
                   dst_rect_.width,
                   dst_rect_.height);
        return false;
    }

    if(!drm_device_)
    {
        DrmDeviceConfig drm_cfg = config_.drm;
        drm_cfg.format = format_;

        drm_device_ = std::make_shared<DrmDevice>(drm_cfg);
    }

    if (!drm_device_->open()) {
        RKCAM_LOGE("[%s] DrmDevice open failed",
                   config_.sink_name.c_str());
        drm_device_.reset();
        return false;
    }

    opened_ = true;

    RKCAM_LOGI("[%s] DrmDisplaySink opened: input=%dx%d fmt=%d dst=(%d,%d,%d,%d) connector=%u crtc=%u plane=%u",
               config_.sink_name.c_str(),
               width_,
               height_,
               static_cast<int>(format_),
               dst_rect_.x,
               dst_rect_.y,
               dst_rect_.width,
               dst_rect_.height,
               drm_device_->connectorId(),
               drm_device_->crtcId(),
               drm_device_->planeId());

    return true;

}


void DrmDisplaySink::close()
{
    if (!opened_) {
        return;
    }

    /*
     * 先释放当前显示 buffer 引用。
     * 然后关闭 DRM plane / 清理 fb cache。
     */
    displayed_buffers_.clear();

    if (drm_device_) {
        drm_device_->close();
        drm_device_.reset();
    }

    width_ = 0;
    height_ = 0;
    format_ = PixelFormat::Unknown;
    dst_rect_ = DrmDisplayRect{};
    opened_ = false;

    RKCAM_LOGI("[%s] DrmDisplaySink closed",
               config_.sink_name.c_str());

}

int DrmDisplaySink::width() const
{
    return width_;
}

int DrmDisplaySink::height() const
{
    return height_;
}

PixelFormat DrmDisplaySink::format() const
{
    return format_;
}

bool DrmDisplaySink::displayFrame(const PipelineVideoFrame& frame)
{
    if (!opened_ || !drm_device_) {
        RKCAM_LOGE("[%s] displayFrame failed: sink is not opened",
                   config_.sink_name.c_str());
        return false;
    }

    if (!validateFrame(frame)) {
        return false;
    }

    DrmFrameDesc desc;
    if(!makeDrmFrameDesc(frame, desc))
    {
        RKCAM_LOGE("[%s] makeDrmFrameDesc failed, frame_id=%lld",
                   config_.sink_name.c_str(),
                   static_cast<long long>(frame.frame_id));
        return false;
    }

    uint32_t fb_id = 0;
    if(!drm_device_ -> importFrameToFb(desc, fb_id))
    {
        RKCAM_LOGE("[%s] importFrameToFb failed, frame_id=%lld",
                   config_.sink_name.c_str(),
                   static_cast<long long>(frame.frame_id));
        return false;
    }
    if(!drm_device_->displayFb(fb_id, dst_rect_, frame.width, frame.height))
    {
        RKCAM_LOGE("[%s] displayFb failed, frame_id=%lld fb_id=%u",
                   config_.sink_name.c_str(),
                   static_cast<long long>(frame.frame_id),
                   fb_id);
        return false;
    }

    /*
     * 非常关键：
     * 当前正在显示的 buffer 不能立刻归还给 pool。
     *
     * 如果 displayFrame 返回后 frame.buffer 被释放，
     * release_cb 可能把 buffer 归还给 DrmBufferPool，
     * 上游下一帧可能马上覆盖它，而 VOP 仍可能正在扫描。
     */
    retainDisplayedBuffer(frame.buffer);
    return true;
}

bool DrmDisplaySink::validateFrame(
    const PipelineVideoFrame& frame) const
{
    if (!frame.buffer) {
        RKCAM_LOGE("[%s] invalid frame: buffer is null",
                   config_.sink_name.c_str());
        return false;
    }

    if (frame.format != format_) {
        RKCAM_LOGE("[%s] frame format mismatch: frame=%d sink=%d",
                   config_.sink_name.c_str(),
                   static_cast<int>(frame.format),
                   static_cast<int>(format_));
        return false;
    }

    if (config_.strict_frame_size &&
        (frame.width != width_ || frame.height != height_)) {
        RKCAM_LOGE("[%s] frame size mismatch: frame=%dx%d sink=%dx%d",
                   config_.sink_name.c_str(),
                   frame.width,
                   frame.height,
                   width_,
                   height_);
        return false;
    }

    if (frame.width <= 0 || frame.height <= 0) {
        RKCAM_LOGE("[%s] invalid frame size=%dx%d",
                   config_.sink_name.c_str(),
                   frame.width,
                   frame.height);
        return false;
    }

    if (frame.buffer->planes.empty()) {
        RKCAM_LOGE("[%s] invalid frame: no planes",
                   config_.sink_name.c_str());
        return false;
    }

    const auto& y_plane = frame.buffer->planes[0];

    if (y_plane.dma_fd < 0) {
        RKCAM_LOGE("[%s] invalid frame: plane[0].dma_fd=%d. "
                   "DrmDisplaySink only accepts DMA frame. "
                   "Use DisplayUploadStage for CPU frames.",
                   config_.sink_name.c_str(),
                   y_plane.dma_fd);
        return false;
    }

    if (y_plane.stride <= 0) {
        RKCAM_LOGE("[%s] invalid frame: plane[0].stride=%d",
                   config_.sink_name.c_str(),
                   y_plane.stride);
        return false;
    }

    return true;
}

bool DrmDisplaySink::makeDrmFrameDesc(const PipelineVideoFrame& frame, DrmFrameDesc& desc) const
{
    desc = DrmFrameDesc{};

    if (frame.format != PixelFormat::NV12) {
        return false;
    }

    if (!frame.buffer || frame.buffer->planes.empty()) {
        return false;
    }

    const auto& y_plane = frame.buffer->planes[0];

    if (y_plane.dma_fd < 0 || y_plane.stride <= 0) {
        return false;
    }

    desc.width = frame.width;
    desc.height = frame.height;
    desc.format = frame.format;

    /*
     * 为了避免 DrmDevice 在 single-plane 情况下用 visible height 推导 UV offset，
     * 这里无论输入是 single-plane 还是 two-plane，都展开成 2 个 DRM plane。
     *
     * 这样可以正确处理：
     *   plane[0].height_stride != frame.height
     * 的情况。
     */
    desc.plane_count = 2;

    desc.planes[0].dma_fd = y_plane.dma_fd;
    desc.planes[0].offset = static_cast<uint32_t>(y_plane.offset);
    desc.planes[0].pitch = static_cast<uint32_t>(y_plane.stride);
    if (frame.buffer->planes.size() >= 2 &&
        frame.buffer->planes[1].dma_fd >= 0) {
        const auto& uv_plane = frame.buffer->planes[1];

        const int uv_stride =
            uv_plane.stride > 0 ? uv_plane.stride : y_plane.stride;

        desc.planes[1].dma_fd = uv_plane.dma_fd;
        desc.planes[1].offset = static_cast<uint32_t>(uv_plane.offset);
        desc.planes[1].pitch = static_cast<uint32_t>(uv_stride);
    }
    else{
        /*
         * NV12 single dma_fd:
         *   Y  offset = y_plane.offset
         *   UV offset = y_plane.offset + stride * height_stride
         */
        const int y_height_stride = y_plane.height_stride > 0 ? y_plane.height_stride : frame.height;

        const uint64_t uv_offset = static_cast<uint64_t>(y_plane.offset) + 
                                   static_cast<uint64_t>(y_plane.stride) *
                                   static_cast<uint64_t>(y_height_stride);
        if(uv_offset > std::numeric_limits<uint32_t>::max()){
            RKCAM_LOGE("[%s] UV offset overflow: offset=%llu",
                       config_.sink_name.c_str(),
                       static_cast<unsigned long long>(uv_offset));
            return false;
        }
        desc.planes[1].dma_fd = y_plane.dma_fd;
        desc.planes[1].offset = static_cast<uint32_t>(uv_offset);
        desc.planes[1].pitch = static_cast<uint32_t>(y_plane.stride);
    }
    return true;

}
void DrmDisplaySink::retainDisplayedBuffer(const std::shared_ptr<VideoBuffer>& buffer)
{
    int hold_count = config_.hold_frame_count;
    if(hold_count <= 0)
    {
        hold_count = 1;
    }
    displayed_buffers_.push_back(buffer);
    while(static_cast<int>(displayed_buffers_.size()) > hold_count)
    {
        displayed_buffers_.pop_front();
    }
}
}