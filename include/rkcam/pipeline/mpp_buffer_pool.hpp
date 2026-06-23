#pragma once

#include "rkcam/pipeline/video_buffer_pool.hpp"

#include "rkcam/video/mpp_rk3568/mpp_buffer.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>

namespace rkcam {

struct MppBufferPoolConfig {
    std::string pool_name = "mpp_buffer_pool";

    int width = 0;
    int height = 0;

    /*
     * 0 表示自动对齐。
     */
    int hor_stride = 0;
    int ver_stride = 0;

    /*
     * MPP 编码通常至少 16 对齐。
     */
    int stride_align = 16;
    int height_align = 16;

    PixelFormat format = PixelFormat::NV12;

    int buffer_count = 4;

    /*
     * 当前 RK 系统有 /dev/dri，所以优先用 DRM 类型。
     */
    MppBufferType buffer_type = MPP_BUFFER_TYPE_DRM;
};
/*
 * 非 owning handle。
 *
 * 真正的 mpp_buffer_put() 由 VideoBuffer::release_cb 执行。
 */
 struct MppNativeBuffer : public NativeBufferHandle{
    MppBuffer buffer = nullptr;
 };
class MppBufferPool final : public IVideoBufferPool, 
    public std:: enable_shared_from_this<MppBufferPool>{
public:
    explicit MppBufferPool(const MppBufferPoolConfig& config);
    ~MppBufferPool() override;

    MppBufferPool(const MppBufferPool&) = delete;
    MppBufferPool& operator=(const MppBufferPool&) = delete;

    bool init() override;
    void close() override;
    std::shared_ptr<VideoBuffer> acquire() override;

    int width() const override { return config_.width; }
    int height() const override { return config_.height; }
    int horStride() const override { return hor_stride_; }
    int verStride() const override { return ver_stride_; }
    PixelFormat format() const override { return config_.format; }
    size_t frameSize() const { return frame_size_; }

private:
    void releaseBuffer(MppBuffer buffer);

    bool calcLayout();

    static int alignTo(int value, int align);

    static int calcPlaneStrideBytes(PixelFormat format, int hor_stride);
    static size_t calcFrameSize(
        PixelFormat format,
        int hor_stride,
        int ver_stride);

private:
    MppBufferPoolConfig config_;

    MppBufferGroup group_ = nullptr;

    int hor_stride_ = 0;
    int ver_stride_ = 0;
    size_t frame_size_ = 0;

    mutable std::mutex mutex_;
    bool stopped_ = false;
    int in_flight_ = 0;
};



} // namespace rkcam