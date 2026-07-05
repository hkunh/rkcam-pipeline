#include "rkcam/platform/linux/drm/drm_buffer_pool.hpp"

#include "rkcam/core/log.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace rkcam {
namespace {

bool isEven(int value)
{
    return (value % 2) == 0;
}
}


DrmBufferPool::DrmBufferPool(const DrmBufferPoolConfig& config) : config_(config)
{

}

DrmBufferPool::~DrmBufferPool()
{
    close();
}


bool DrmBufferPool::validateConfig() const
{
    if (config_.width <= 0 || config_.height <= 0) {
        RKCAM_LOGE("[%s] invalid size: %dx%d",
                   config_.pool_name.c_str(),
                   config_.width,
                   config_.height);
        return false;
    }

    if (config_.format != PixelFormat::NV12) {
        RKCAM_LOGE("[%s] only NV12 is supported now, format=%d",
                   config_.pool_name.c_str(),
                   static_cast<int>(config_.format));
        return false;
    }

    /*
     * NV12 要求宽高至少是偶数。
     */
    if (!isEven(config_.width) || !isEven(config_.height)) {
        RKCAM_LOGE("[%s] NV12 requires even width/height, got %dx%d",
                   config_.pool_name.c_str(),
                   config_.width,
                   config_.height);
        return false;
    }

    if (config_.buffer_count <= 0) {
        RKCAM_LOGE("[%s] invalid buffer_count=%d",
                   config_.pool_name.c_str(),
                   config_.buffer_count);
        return false;
    }

    if (config_.device.empty()) {
        RKCAM_LOGE("[%s] drm device is empty",
                   config_.pool_name.c_str());
        return false;
    }

    return true;
}

bool DrmBufferPool::init()
{
    if(initialized_)
    {
        return true;
    }

    if (!validateConfig()) {
        return false;
    }

    fd_ = ::open(config_.device.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        RKCAM_LOGE("[%s] open DRM device failed: %s errno=%d(%s)",
                   config_.pool_name.c_str(),
                   config_.device.c_str(),
                   errno,
                   std::strerror(errno));
        return false;
    }

    width_ = config_.width;
    height_ = config_.height;
    height_stride_ = height_;

    slots_.resize(static_cast<size_t>(config_.buffer_count));


    for(int i = 0; i < config_.buffer_count; i++)
    {
        if(!allocateOneSlot(i, slots_[static_cast<size_t>(i)]))
        {
            RKCAM_LOGE("[%s] allocate slot %d failed",
                       config_.pool_name.c_str(),
                       i);
            destroyAllSlots();
            ::close(fd_);
            fd_ = -1;
            return false;
        }
    }


    if (!slots_.empty()) {
        stride_ = slots_[0].stride;
        buffer_size_ = slots_[0].size;
    }

    initialized_ = true;

    RKCAM_LOGI("[%s] init done: %dx%d stride=%d height_stride=%d size=%zu count=%d map_cpu=%d",
               config_.pool_name.c_str(),
               width_,
               height_,
               stride_,
               height_stride_,
               buffer_size_,
               config_.buffer_count,
               config_.map_cpu ? 1 : 0);

    return true;
}

void DrmBufferPool::close(){
    destroyAllSlots();
    if(fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
    initialized_ = false;
}


bool DrmBufferPool::allocateOneSlot(int index, Slot& slot)
{
    slot = Slot{};
    slot.index = index;
    slot.width = config_.width;
    slot.height = config_.height;
    slot.height_stride = config_.height;

    /*
     * NV12 dumb buffer 推荐写法：
     *
     *   create.width  = width
     *   create.height = height * 3 / 2
     *   create.bpp    = 8
     *
     * 不建议直接 bpp=12，因为部分 DRM 驱动对 dumb buffer bpp 支持不完整。
     */

    drm_mode_create_dumb create {};
    create.width = static_cast<uint32_t>(config_.width);
    create.height = static_cast<uint32_t>(config_.height * 3/2);
    create.bpp = 8;
    if(drmIoctl(fd_, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0)
    {
        RKCAM_LOGE("[%s] DRM_IOCTL_MODE_CREATE_DUMB failed index=%d errno=%d(%s)",
                   config_.pool_name.c_str(),
                   index,
                   errno,
                   std::strerror(errno));
        return false;
    }

    slot.gem_handle = create.handle;
    slot.stride = static_cast<int>(create.pitch);
    slot.size = static_cast<size_t>(create.size);
    slot.uv_offset = static_cast<size_t>(slot.stride) * static_cast<size_t>(config_.height);


    /*
     * 导出 dma_fd。
     * 这个 fd 后续给 RGA importbuffer_fd() 使用，
     * 也给 DrmDevice::importFrameToFb() 使用。
     */
    int dma_fd = -1;
    if(drmPrimeHandleToFD(
        fd_,
        slot.gem_handle,
        DRM_CLOEXEC | DRM_RDWR,
        &dma_fd
    ) != 0)
    {
        RKCAM_LOGE("[%s] drmPrimeHandleToFD failed index=%d handle=%u errno=%d(%s)",
                   config_.pool_name.c_str(),
                   index,
                   slot.gem_handle,
                   errno,
                   std::strerror(errno));
        drm_mode_destroy_dumb destroy{};
        destroy.handle = slot.gem_handle;

        drmIoctl(fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);

        slot = Slot{};
        return false;
    }

    slot.dma_fd = dma_fd;

    if(config_.map_cpu)
    {
        drm_mode_map_dumb map_req{};
        map_req.handle = slot.gem_handle;
        if(drmIoctl(fd_, DRM_IOCTL_MODE_MAP_DUMB, &map_req) != 0)
        {
            RKCAM_LOGE("[%s] DRM_IOCTL_MODE_MAP_DUMB failed index=%d errno=%d(%s)",
                       config_.pool_name.c_str(),
                       index,
                       errno,
                       std::strerror(errno));

            destroyOneSlot(slot);
            return false;
        }

        slot.map = mmap(
            nullptr,
            slot.size,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            fd_,
            map_req.offset
        );
        if (slot.map == MAP_FAILED) {
            RKCAM_LOGE("[%s] mmap DRM dumb buffer failed index=%d errno=%d(%s)",
                       config_.pool_name.c_str(),
                       index,
                       errno,
                       std::strerror(errno));
            slot.map = nullptr;
            destroyOneSlot(slot);
            return false;
        }

        if (config_.clear_on_init) {
            std::memset(slot.map, 0, slot.size);
        }

    }

    RKCAM_LOGI("[%s] slot[%d] allocated: handle=%u dma_fd=%d %dx%d stride=%d uv_offset=%zu size=%zu map=%p",
               config_.pool_name.c_str(),
               index,
               slot.gem_handle,
               slot.dma_fd,
               slot.width,
               slot.height,
               slot.stride,
               slot.uv_offset,
               slot.size,
               slot.map);

    return true;
}

void DrmBufferPool::destroyOneSlot(Slot& slot)
{
    if (slot.map) {
        munmap(slot.map, slot.size);
        slot.map = nullptr;
    }

    if (slot.dma_fd >= 0) {
        ::close(slot.dma_fd);
        slot.dma_fd = -1;
    }
    if (fd_ >= 0 && slot.gem_handle != 0) {
        drm_mode_destroy_dumb destroy {};
        destroy.handle = slot.gem_handle;

        if (drmIoctl(fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy) != 0) {
            RKCAM_LOGE("[%s] DRM_IOCTL_MODE_DESTROY_DUMB failed index=%d handle=%u errno=%d(%s)",
                       config_.pool_name.c_str(),
                       slot.index,
                       slot.gem_handle,
                       errno,
                       std::strerror(errno));
        }

        slot.gem_handle = 0;
    }

    slot = Slot{};
}
void DrmBufferPool::destroyAllSlots()
{
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& slot : slots_) {
        destroyOneSlot(slot);
    }

    slots_.clear();

    stride_ = 0;
    height_stride_ = 0;
    buffer_size_ = 0;
}

std::shared_ptr<VideoBuffer> DrmBufferPool::acquire()
{
    if(!initialized_)
    {
        RKCAM_LOGE("[%s] acquire failed: pool is not initialized",
                   config_.pool_name.c_str());
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    for(size_t i = 0; i < slots_.size(); i++)
    {
        Slot& slot = slots_[i];
        if(slot.in_use)
        {
            continue;
        }
        slot.in_use = true;
        return wrapSlotAsVideoBuffer(static_cast<int>(i));
    }

    RKCAM_LOGE("[%s] acquire failed: no free buffer",
               config_.pool_name.c_str());
    return nullptr;
}

std::shared_ptr<VideoBuffer> DrmBufferPool::wrapSlotAsVideoBuffer(int index)
{
    if (index < 0 || index >= static_cast<int>(slots_.size())) {
        return nullptr;
    }

    Slot& slot = slots_[static_cast<size_t>(index)];

    auto buffer = std::make_shared<VideoBuffer>();
    buffer->memory_type = VideoMemoryType::DmaBuffer;

    
    /*
     * 这里建议第一版用 single-plane NV12 表达：
     *
     *   plane[0] = 整块 NV12 buffer
     *   Y  起始 offset = 0
     *   UV 起始 offset = stride * height_stride
     *
     * 好处：
     *   1. 更接近 V4L2 mainpath 当前 num_planes=1 的情况；
     *   2. RGA 输出到 NV12 dma_fd 时更容易处理；
     *   3. DrmDevice 已经支持 plane_count=1 时自动推导 UV offset。
     */
    buffer->planes.resize(1);
    VideoBufferPlane& plane = buffer->planes[0];
    plane.data = config_.map_cpu && slot.map ? static_cast<uint8_t*>(slot.map) : nullptr;

    plane.dma_fd = slot.dma_fd;
    plane.offset = 0;

    plane.stride = slot.stride;
    plane.height_stride = slot.height_stride;

    plane.length = slot.size;
    plane.bytesused = slot.size;

    auto native = std::make_shared<DrmBufferNativeHandle>();
    native->slot_index = index;
    native->drm_fd = fd_;
    native->gem_handle = slot.gem_handle;
    native->dma_fd = slot.dma_fd;
    native->width = slot.width;
    native->height = slot.height;
    native->stride = slot.stride;
    native->uv_offset = slot.uv_offset;
    native->size = slot.size;
    native->format = config_.format;

    buffer->native_handle = native;

    /*
     * 关键：
     * buffer 析构时不销毁 DRM 资源，只归还 slot。
     *
     * 捕获 shared_from_this() 是为了保证：
     *   即使 RgaStage 里 output_pool_.reset()，
     *   下游还持有 VideoBuffer 时，pool 不会提前析构。
     */
    std::shared_ptr<DrmBufferPool> self = shared_from_this();
    buffer->release_cb = [self, index](){
        self->releaseSlot(index);
    };
    return buffer;    

}


void DrmBufferPool::releaseSlot(int index)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (index < 0 || index >= static_cast<int>(slots_.size())) {
        RKCAM_LOGE("[%s] release invalid slot index=%d",
                   config_.pool_name.c_str(),
                   index);
        return;
    }

    Slot& slot = slots_[static_cast<size_t>(index)];
    if(!slot.in_use)
    {
        RKCAM_LOGE("[%s] release slot[%d] but it is not in use",
                   config_.pool_name.c_str(),
                   index);
        return;
    }

    slot.in_use = false;


}
int DrmBufferPool::width() const
{
    return width_;
}

int DrmBufferPool::height() const
{
    return height_;
}

int DrmBufferPool::horStride() const
{
    return stride_;
}

int DrmBufferPool::verStride() const
{
    return height_stride_;
}

PixelFormat DrmBufferPool::format() const
{
    return config_.format;
}

size_t DrmBufferPool::bufferSize() const
{
    return buffer_size_;
}

int DrmBufferPool::bufferCount() const
{
    return static_cast<int>(slots_.size());
}


}