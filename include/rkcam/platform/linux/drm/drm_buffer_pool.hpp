#pragma once

#include "rkcam/video/video_buffer_pool.hpp"
#include "rkcam/video/video_frame.hpp"
#include "rkcam/video/video_types.hpp"

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// 1.【分配阶段】DrmBufferPool 
//    调用 CREATE_DUMB 得到【本地 gem_handle】
//      │
//      ▼ (调用 drmPrimeHandleToFD)
//    导出为【通用 dma_fd】 ───► 封装进 VideoBuffer 中流转
//                                │
//                                ▼
// 2.【加工阶段】RGA Processor 收到 VideoBuffer
//    直接把【通用 dma_fd】塞给 RGA 硬件 ───► RGA 往里面疯狂写 NV12 图像数据
//    (RGA 驱动完全不需要知道什么是 DRM，实现了绝对解耦！✓)
//                                │
//                                ▼
// 3.【显示阶段】DrmDevice 收到写满数据的 VideoBuffer
//    为了能调用 drmModeAddFB2 刷屏，它必须向内核申请自己上下文里的句柄
//      │
//      ▼ (调用 drmPrimeFDToHandle)
//    把【通用 dma_fd】导入为 DrmDevice 自己的【本地 handle_y / handle_uv】
//      │
//      ▼ (调用 drmModeAddFB2)
//    生成最终的 fb_id 送显！

namespace rkcam{

struct DrmBufferPoolConfig{
    std::string pool_name = "drm_buffer_pool";
    std::string device = "/dev/dri/card0";

    int width = 0;
    int height = 0;
   
    /*
     * 第一版只支持 NV12。
     */
    PixelFormat format = PixelFormat::NV12;

    int buffer_count = 3;

    /*
     * CPU copy 版本需要 mmap。
     * 后面 RGA 直接写 DRM buffer 时，可以设为 false。
     */
     bool map_cpu = true;

    /*
     * 初始化时是否清黑。
     */
    bool clear_on_init = true;

};


struct DrmBufferNativeHandle : public NativeBufferHandle{
    int slot_index = -1;

    //注意：结构体里记着 drm_fd 和 gem_handle，纯粹是留着最后销户（释放内存）用的
    int drm_fd = -1; //设备的通道 代表你和哪张显卡建立了连接
    uint32_t gem_handle = 0;
    int dma_fd = -1;

    int width = 0;
    int height = 0;
    int stride = 0;
    size_t uv_offset = 0;
    size_t size = 0;

    PixelFormat format = PixelFormat::Unknown;
};
class DrmBufferPool : public IVideoBufferPool, public std::enable_shared_from_this<DrmBufferPool>{

public:
    explicit DrmBufferPool(const DrmBufferPoolConfig& config);
    ~DrmBufferPool() override;

    DrmBufferPool(const DrmBufferPool&) = delete;
    DrmBufferPool& operator=(const DrmBufferPool&) =delete;

    bool init();

    std::shared_ptr<VideoBuffer> acquire() override;

    int width() const override;
    int height() const override;

    void close() override;

    /*
     * 这里返回 bytes per line。
     * 对 NV12 来说，也是 Y plane pitch。
     */
    int horStride() const override;

    /*
     * 对 NV12 来说，返回 Y plane 的行数。
     */
    int verStride() const override;

    PixelFormat format() const override;

    size_t bufferSize() const;
    int bufferCount() const;
private:
    struct Slot{
        int index = -1;
        bool in_use = false;

        uint32_t gem_handle = 0;
        int dma_fd = -1;

        int width = 0;
        int height = 0;

        int stride = 0;
        int height_stride = 0;

        size_t uv_offset = 0;
        size_t size = 0;

        void* map = nullptr;

    };
private:
    bool allocateOneSlot(int index, Slot& slot);
    void destroyOneSlot(Slot& slot);
    void destroyAllSlots();

    void releaseSlot(int index);

    std::shared_ptr<VideoBuffer> wrapSlotAsVideoBuffer(int index);

    bool validateConfig() const;

private:
    DrmBufferPoolConfig config_;

    int fd_ = -1;

    int width_ = 0;
    int height_ = 0;
    int stride_ = 0;
    int height_stride_ = 0;
    size_t buffer_size_ = 0;

    std::vector<Slot> slots_;

    mutable std::mutex mutex_;

    bool initialized_ = false;


};



}