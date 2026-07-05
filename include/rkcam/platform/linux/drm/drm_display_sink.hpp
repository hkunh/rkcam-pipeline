#pragma once

#include "rkcam/display/display_sink.hpp"
#include "rkcam/platform/linux/drm/drm_device.hpp"
#include "rkcam/platform/linux/drm/drm_types.hpp"
#include "rkcam/video/video_frame.hpp"
#include "rkcam/video/video_types.hpp"

#include <deque>
#include <memory>
#include <string>

namespace rkcam {

struct DrmDisplaySinkConfig{

    std::string sink_name = "drm_display";

    /*
     * DrmDevice 负责：
     *   open /dev/dri/card0
     *   dma_fd -> fb_id import/cache
     *   drmModeSetPlane()
     */
    
    DrmDeviceConfig drm;

    /*
     * 输入 frame 尺寸。
     * 第一版建议 RGA / DisplayUploadStage 输出 1080x1920 NV12。
     */
    int width = 1080;
    int height = 1920;

    PixelFormat format = PixelFormat::NV12;

    /*
     * 显示区域。
     * width/height <= 0 时，默认全尺寸显示。
     */
    DrmDisplayRect dst_rect;

    /*
     * 至少持有最近 1~2 帧，避免 displayFrame 返回后 buffer 被 pool 立即回收。
     */
    int hold_frame_count = 2;

    /*
     * 第一版建议严格检查 frame 尺寸必须匹配 config。
     */
    bool strict_frame_size = true;

};


class DrmDisplaySink : public IDisplaySink{
public:
    explicit DrmDisplaySink(const DrmDisplaySinkConfig& config);
    /*
     * 可选：外部传入 DrmDevice，方便后面 UI/多 plane 共享同一个 DRM device。
     */
    DrmDisplaySink(const DrmDisplaySinkConfig& config, std::shared_ptr<DrmDevice> device);
    ~DrmDisplaySink() override;

    DrmDisplaySink(const DrmDisplaySink&) = delete;
    DrmDisplaySink& operator=(const DrmDisplaySink&) = delete;

    bool open() override;
    void close() override;

    int width() const override;
    int height() const override;
    PixelFormat format() const override;

    bool displayFrame(const PipelineVideoFrame& frame) override;

private:
    bool makeDrmFrameDesc(const PipelineVideoFrame& frame, DrmFrameDesc& desc) const;
    
    
     bool validateFrame(
        const PipelineVideoFrame& frame) const;

    void retainDisplayedBuffer(
        const std::shared_ptr<VideoBuffer>& buffer);   
    
private:
    DrmDisplaySinkConfig config_;
    std::shared_ptr<DrmDevice> drm_device_;

    int width_ = 0;
    int height_ = 0;
    PixelFormat format_ = PixelFormat::Unknown;

    DrmDisplayRect dst_rect_;

    std::deque<std::shared_ptr<VideoBuffer>> displayed_buffers_;

    bool opened_ = false;

};
    
}