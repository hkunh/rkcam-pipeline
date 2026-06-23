#pragma once

#include "rkcam/pipeline/pipeline_video_frame.hpp"

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rkcam {

struct DmaBufferPoolConfig {
    std::string pool_name = "dma_pool";

    /*
     * 常见：
     *   /dev/dma_heap/system
     *   /dev/dma_heap/cma
     */
    std::string heap_path = "/dev/dma_heap/system";

    int width = 0;
    int height = 0;

    /*
     * NV12:
     *   一帧一个 dma-buf plane:
     *     plane[0] = Y + UV
     *
     * NV12M:
     *   一帧两个 dma-buf plane:
     *     plane[0] = Y
     *     plane[1] = UV
     *
     * RGB888/BGR888:
     *   一帧一个 dma-buf plane
     */
    PixelFormat format = PixelFormat::NV12;

    int buffer_count = 4;

    /*
     * true:
     *   对每个 dma-buf 做 mmap，方便 RawSaveStage 调试保存。
     *
     * false:
     *   只提供 dma_fd，适合 MPP / RGA / Display 纯硬件链路。
     */
    bool mmap_cpu = false;
};

class DmaBufferPool : public std::enable_shared_from_this<DmaBufferPool> {
public:
    explicit DmaBufferPool(const DmaBufferPoolConfig& config);
    ~DmaBufferPool();

    DmaBufferPool(const DmaBufferPool&) = delete;
    DmaBufferPool& operator=(const DmaBufferPool&) = delete;

    bool init();
    void close();

    /*
     * 获取一帧 DMA buffer。
     *
     * 返回的 VideoBuffer::planes.size() 由 format 决定：
     *   NV12  -> 1
     *   NV12M -> 2
     *   RGB   -> 1
     */
    std::shared_ptr<VideoBuffer> acquire();

    int width() const { return config_.width; }
    int height() const { return config_.height; }
    PixelFormat format() const { return config_.format; }

    size_t planeCount() const;

private:
    struct PlaneSlot {
        int fd = -1;
        void* mmap_addr = nullptr;

        size_t size = 0;
        size_t bytesused = 0;
        int stride = 0;
    };

    struct Slot {
        std::vector<PlaneSlot> planes;
        bool in_use = false;
    };

private:
    bool allocSlot(Slot& slot, size_t slot_index);
    bool fillPlaneSlotLayout(size_t plane_index, PlaneSlot& plane) const;

    int allocDmaHeapBuffer(size_t size);
    void releaseSlot(size_t index);

    bool hasInUseLocked() const;

    static size_t planeCountForFormat(PixelFormat format);

    static int alignEven(int value);
    static int alignTo(int value, int align);

private:
    DmaBufferPoolConfig config_;

    int heap_fd_ = -1;

    std::mutex mutex_;
    std::condition_variable cv_;

    std::vector<Slot> slots_;

    bool stopped_ = false;
};

} // namespace rkcam