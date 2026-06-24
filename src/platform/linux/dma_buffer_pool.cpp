#include "rkcam/pipeline/dma_buffer_pool.hpp"
#include "rkcam/core/log.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace rkcam {

DmaBufferPool::DmaBufferPool(const DmaBufferPoolConfig& config)
    : config_(config)
{
}

DmaBufferPool::~DmaBufferPool()
{
    close();
}

bool DmaBufferPool::init()
{
    if (config_.width <= 0 ||
        config_.height <= 0 ||
        config_.buffer_count <= 0) {
        RKCAM_LOGE("[%s] invalid config: width=%d height=%d count=%d",
                   config_.pool_name.c_str(),
                   config_.width,
                   config_.height,
                   config_.buffer_count);
        return false;
    }

    const size_t plane_count = planeCountForFormat(config_.format);
    if (plane_count == 0) {
        RKCAM_LOGE("[%s] unsupported format=%d",
                   config_.pool_name.c_str(),
                   static_cast<int>(config_.format));
        return false;
    }

    heap_fd_ = ::open(config_.heap_path.c_str(), O_RDWR | O_CLOEXEC);
    if (heap_fd_ < 0) {
        RKCAM_LOGE("[%s] open dma heap failed: %s, path=%s",
                   config_.pool_name.c_str(),
                   std::strerror(errno),
                   config_.heap_path.c_str());
        return false;
    }

    slots_.resize(static_cast<size_t>(config_.buffer_count));

    for (size_t i = 0; i < slots_.size(); ++i) {
        if (!allocSlot(slots_[i], i)) {
            RKCAM_LOGE("[%s] alloc slot failed, index=%zu",
                       config_.pool_name.c_str(),
                       i);
            close();
            return false;
        }
    }

    stopped_ = false;

    RKCAM_LOGI("[%s] DmaBufferPool init done: width=%d height=%d format=%d count=%d planes=%zu mmap_cpu=%d",
               config_.pool_name.c_str(),
               config_.width,
               config_.height,
               static_cast<int>(config_.format),
               config_.buffer_count,
               plane_count,
               config_.mmap_cpu ? 1 : 0);

    return true;
}


void DmaBufferPool::close()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;

        /*
         * 如果还有下游持有 VideoBuffer，不要强行 close fd。
         * 正常情况下 VideoBuffer::release_cb 捕获 shared_ptr<DmaBufferPool>，
         * 所以 pool 会等 buffer 都归还后才析构。
         */
        if (hasInUseLocked()) {
            RKCAM_LOGE("[%s] close called while buffers still in use, skip closing now",
                       config_.pool_name.c_str());
            cv_.notify_all();
            return;
        }
    }

    cv_.notify_all();

    for (auto& slot : slots_) {
        for (auto& plane : slot.planes) {
            if (plane.mmap_addr) {
                ::munmap(plane.mmap_addr, plane.size);
                plane.mmap_addr = nullptr;
            }

            if (plane.fd >= 0) {
                ::close(plane.fd);
                plane.fd = -1;
            }

            plane.size = 0;
            plane.bytesused = 0;
            plane.stride = 0;
        }

        slot.planes.clear();
        slot.in_use = false;
    }

    slots_.clear();

    if (heap_fd_ >= 0) {
        ::close(heap_fd_);
        heap_fd_ = -1;
    }
}

std::shared_ptr<VideoBuffer> DmaBufferPool::acquire()
{
    std::unique_lock<std::mutex> lock(mutex);

    cv_.wait(lock, [this](){
        if(stooped_)
        {
            return true;
        }
        for(const auto& slot : slots_){
            if(!slot.in_use)
            {
                return true;
            }
        }
    });
    if(stopped_)
    {
        return nullptr;
    }
    size_t index = slots_.size();

    for(size_t i = 0; i < slots_.size(); i++)
    {
        if(!slots_[i].in_use)
        {
            index = i;
            break;
        }
    }

    if(index >= slots.size())
    {
        return nullptr;
    }
    Slot& slot = slots_[index];
    slot.in_use = true;

    auto buffer = std::make_shared<VideoBuffer>();
    buffer->memory_type = VideoMemoryType::DmaBuffer;
    buffer->planes.resize(slot.planes.size());

    for(size_t p = 0; p < slot.planes.size(); ++p)
    {
        const auto& sp = slot.planes[p];
        auto& dp =buffer->planes[p];
        dp.data = static_cast<uint8_t*>(sp.mmap_addr);
        dp.cpu_storeage.clear();

        dp.dma_fd = sp.fd;
        dp.offset = 0;

        dp.length = sp.size;
        dp.bytesuesd = sp.bytesused;
        dp.stide = sp.stride;
    }
    /*
     * 关键：
     *   release_cb 捕获 shared_ptr<DmaBufferPool>。
     *   所以下游还持有 buffer 时，pool 不会提前析构。
     */

    auto pool = shared_from_this();
    buffer->release_cb = [pool, index](){
        pool->releaseSlot(index);
    };
    return buffer;
}




bool DmaBufferPool::allocSlot(Slot& slot, size_t slot_index)
{
    const size_t plane_count = planeCountForFrame(config_.format);
    if(plane_count == 0)
    {
        return false;
    }
    slot.planes.clear();
    slot.planes.resize(plane_count);
    slot.in_use = false;


    for(size_t p = 0; p < plane_count; p++)
    {
        auto& plane = slot.planes[p];
        if(!fillPlaneSlotLayout(p, plane))
        {
            RKCAM_LOGE("[%s] fillPlaneSlotLayout failed, slot=%zu plane=%zu",
                       config_.pool_name.c_str(),
                       slot_index,
                       p);
            return false;
        }
        plane.fd = allocDmaHeapBuffer(plane.size);
        if (plane.fd < 0) {
            RKCAM_LOGE("[%s] alloc dma buffer failed, slot=%zu plane=%zu size=%zu",
                       config_.pool_name.c_str(),
                       slot_index,
                       p,
                       plane.size);
            return false;
        }
        if(config_.mmap_cpu)
        {
            plane.mmap_addr = ::mmap(
                nullptr,
                plane.size,
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                plane.fd,
                0
            );
            if (plane.mmap_addr == MAP_FAILED) {
                RKCAM_LOGE("[%s] mmap failed: slot=%zu plane=%zu fd=%d size=%zu error=%s",
                           config_.pool_name.c_str(),
                           slot_index,
                           p,
                           plane.fd,
                           plane.size,
                           std::strerror(errno));
                plane.mmap_addr = nullptr;
                return false;
            }
        }
        RKCAM_LOGI("[%s] alloc slot[%zu].plane[%zu]: fd=%d size=%zu bytesused=%zu stride=%d mmap=%p",
                   config_.pool_name.c_str(),
                   slot_index,
                   p,
                   plane.fd,
                   plane.size,
                   plane.bytesused,
                   plane.stride,
                   plane.mmap_addr);
    }
    return true;
}


bool DmaBufferPool::fillPlaneSlotLayout(
    size_t plane_index,
    PlaneSlot& plane) const
{
    const int width = config_.width;
    const int height = config_.height;

    if (width <= 0 || height <= 0) {
        return false;
    }

    plane.size = 0;
    plane.bytesused = 0;
    plane.stride = 0;

    switch (config_.format) {
    case PixelFormat::NV12: {
        /*
         * NV12 single memory-plane:
         *   plane[0] = Y + UV
         */
        if (plane_index != 0) {
            return false;
        }

        const int stride = alignEven(width);
        const int aligned_height = alignEven(height);

        plane.stride = stride;
        plane.size =
            static_cast<size_t>(stride) *
            static_cast<size_t>(aligned_height) *
            3 / 2;
        plane.bytesused = plane.size;

        return true;
    }

    case PixelFormat::RGB888:
    case PixelFormat::BGR888: {
        /*
         * RGB/BGR single memory-plane。
         * 当前先不额外做 16/64 字节对齐。
         * 如果后面 RGA/MPP 有要求，再把 stride 改成 alignTo(width * 3, 16/64)。
         */
        if (plane_index != 0) {
            return false;
        }

        const int stride = width * 3;

        plane.stride = stride;
        plane.size =
            static_cast<size_t>(stride) *
            static_cast<size_t>(height);
        plane.bytesused = plane.size;

        return true;
    }

    default:
        return false;
    }
}

int DmaBufferPool::allocDmaHeapBuffer(size_t size)
{
    if(heap_fd_ < 0 || size == 0)
    {
        return -1;
    }
    dma_heap_allocation_data data{};
    data.len = size;
    data.fd_flags = O_RDWR | O_CLOEXEC;
    data.heap_flags = 0;
    if(::ioctl(heap_fd_, DMA_HEAP_IOCTL_ALLOC, &data) < 0)
    {
        RKCAM_LOGE("[%s] DMA_HEAP_IOCTL_ALLOC failed: size=%zu error=%s",
                   config_.pool_name.c_str(),
                   size,
                   std::strerror(errno));
        return -1;
    }
    return data.fd;
}


void DmaBufferPool::releaseSlot(size_t index)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (index >= slots_.size()) {
            RKCAM_LOGE("[%s] releaseSlot invalid index=%zu",
                       config_.pool_name.c_str(),
                       index);
            return;
        }

        if (!slots_[index].in_use) {
            RKCAM_LOGE("[%s] releaseSlot double free index=%zu",
                       config_.pool_name.c_str(),
                       index);
            return;
        }

        slots_[index].in_use = false;
    }

    cv_.notify_one();
}

bool DmaBufferPool::hasInUseLocked() const
{
    for (const auto& slot : slots_) {
        if (slot.in_use) {
            return true;
        }
    }

    return false;
}

size_t DmaBufferPool::planeCount() const
{
    return planeCountForFormat(config_.format);
}

size_t DmaBufferPool::planeCount() const
{
    return planeCountForFormat(config_.format);
}

size_t DmaBufferPool::planeCountForFormat(PixelFormat format)
{
    switch (format) {
    case PixelFormat::NV12:
        return 1;

    case PixelFormat::NV12M:
        return 2;

    case PixelFormat::RGB888:
    case PixelFormat::BGR888:
        return 1;

    default:
        return 0;
    }
}

int DmaBufferPool::alignEven(int value)
{
    if (value <= 0) {
        return 0;
    }

    return (value + 1) & ~1;
}

int DmaBufferPool::alignTo(int value, int align)
{
    if (value <= 0 || align <= 0) {
        return 0;
    }

    return (value + align - 1) / align * align;
}

}