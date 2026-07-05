#pragma once

#include "rkcam/video/video_frame.hpp"
#include "rkcam/video/video_types.hpp"
namespace rkcam{
class IDisplaySink{
public:
    virtual ~IDisplaySink() = default;

    virtual bool open() = 0;
    virtual void close() = 0;
    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual PixelFormat format() const = 0;

    /*
     * displayFrame 比 present 更符合当前相机 pipeline 语义。
     *
     * CpuCopy 模式：
     *   frame 是普通 CPU/DMA 映射后的 NV12 frame；
     *   sink 内部 copy 到 DRM buffer 后显示。
     *
     * DrmBuffer 模式：
     *   frame.buffer->native_handle 里有 DrmBufferHandle；
     *   sink 直接显示 handle->fb_id。
     */
    virtual bool displayFrame(const PipelineVideoFrame& frame) = 0;
};
}