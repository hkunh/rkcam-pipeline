#include "rkcam/platform/rockchip/mpp/mpp_buffer_pool.hpp"
#include "rkcam/core/log.hpp"

#include <cstring>

namespace rkcam{


MppBufferPool::MppBufferPool(const MppBufferPoolConfig& config)
    : config_(config)
{
}

MppBufferPool::~MppBufferPool()
{
    close();
}

bool MppBufferPool::init()
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

    if (!calcLayout()) {
        RKCAM_LOGE("[%s] calcLayout failed, format=%d",
                   config_.pool_name.c_str(),
                   static_cast<int>(config_.format));
        return false;
    }

    MPP_RET ret = mpp_buffer_group_get_internal(
        &group_,
        config_.buffer_type);

    if (ret != MPP_OK || !group_) {
        RKCAM_LOGE("[%s] mpp_buffer_group_get_internal failed, ret=%d type=%d",
                   config_.pool_name.c_str(),
                   ret,
                   static_cast<int>(config_.buffer_type));
        group_ = nullptr;
        return false;
    }
    /*
     * 限制 group 的 buffer size 和数量。
     */
    ret = mpp_buffer_group_limit_config(
        group_,
        frame_size_,
        config_.buffer_count);

    if (ret != MPP_OK) {
        RKCAM_LOGE("[%s] mpp_buffer_group_limit_config failed, ret=%d size=%zu count=%d",
                   config_.pool_name.c_str(),
                   ret,
                   frame_size_,
                   config_.buffer_count);
        close();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = false;
        in_flight_ = 0;
    }

    RKCAM_LOGI("[%s] init done: width=%d height=%d hor_stride=%d ver_stride=%d format=%d frame_size=%zu count=%d type=%d",
               config_.pool_name.c_str(),
               config_.width,
               config_.height,
               hor_stride_,
               ver_stride_,
               static_cast<int>(config_.format),
               frame_size_,
               config_.buffer_count,
               static_cast<int>(config_.buffer_type));

    return true;
}

void MppBufferPool::close()
{
    MppBufferGroup group_to_put = nullptr;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        stopped_ = true;

        /*
         * 如果还有下游持有 VideoBuffer，不能马上 put group。
         * 因为 release_cb 里还要 mpp_buffer_put(buffer)。
         *
         * 由于 release_cb 捕获 shared_ptr<MppBufferPool>，
         * 这个 pool 会一直活到最后一个 VideoBuffer 释放。
         */
        if (in_flight_ != 0) {
            RKCAM_LOGI("[%s] close delayed, in_flight=%d",
                       config_.pool_name.c_str(),
                       in_flight_);
            return;
        }

        group_to_put = group_;
        group_ = nullptr;
    }

    if (group_to_put) {
        mpp_buffer_group_put(group_to_put);
        RKCAM_LOGI("[%s] group put", config_.pool_name.c_str());
    }
}

std::shared_ptr<VideoBuffer> MppBufferPool::acquire()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) {
            RKCAM_LOGE("[%s] acquire failed: pool stopped",
                       config_.pool_name.c_str());
            return nullptr;
        }

        if (!group_) {
            RKCAM_LOGE("[%s] acquire failed: group is null",
                       config_.pool_name.c_str());
            return nullptr;
        }
    }
    MppBuffer mpp_buf = nullptr;
    MPP_RET ret = mpp_buffer_get(
        group_,
        &mpp_buf,
        frame_size_);

    if (ret != MPP_OK || !mpp_buf) {
        RKCAM_LOGE("[%s] mpp_buffer_get failed, ret=%d size=%zu",
                   config_.pool_name.c_str(),
                   ret,
                   frame_size_);
        return nullptr;
    }

    const int fd = mpp_buffer_get_fd(mpp_buf);
    void* ptr = mpp_buffer_get_ptr(mpp_buf);
    const size_t real_size = mpp_buffer_get_size(mpp_buf);
    if (fd < 0) {
        RKCAM_LOGE("[%s] mpp_buffer_get_fd failed, fd=%d",
                   config_.pool_name.c_str(),
                   fd);
        mpp_buffer_put(mpp_buf);
        return nullptr;
    }

    if (real_size < frame_size_) {
        RKCAM_LOGE("[%s] mpp buffer too small: real_size=%zu required=%zu",
                   config_.pool_name.c_str(),
                   real_size,
                   frame_size_);
        mpp_buffer_put(mpp_buf);
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++in_flight_;
    }

    auto buffer = std::make_shared<VideoBuffer>();
    buffer->memory_type = VideoMemoryType::DmaBuffer;
    buffer->planes.resize(1);


    auto& plane = buffer->planes[0];

    plane.dma_fd = fd;
    plane.data = static_cast<uint8_t*>(ptr);
    plane.cpu_storage.clear();

    plane.offset = 0;
    plane.length = real_size;
    plane.bytesused = frame_size_;
    plane.stride = calcPlaneStrideBytes(config_.format, hor_stride_);
    plane.height_stride = ver_stride_;

    auto native = std::make_shared<MppNativeBuffer>();
    native->buffer = mpp_buf;
    buffer->native_handle = native;


    /*
     * 关键：
     *   不是 RGA 用完就 put。
     *   是最后一个持有这个 VideoBuffer 的下游释放时 put。
     */
    auto pool = shared_from_this();
    buffer->release_cb = [pool, mpp_buf](){
        pool->releaseBuffer(mpp_buf);
    };

    // RKCAM_LOGI("[%s] acquire buffer: fd=%d ptr=%p size=%zu frame_size=%zu in_flight=%d",
    //            config_.pool_name.c_str(),
    //            fd,
    //            ptr,
    //            real_size,
    //            frame_size_,
    //            in_flight_);

    return buffer;
}
void MppBufferPool::releaseBuffer(MppBuffer buffer)
{
    if(!buffer)
    {
        return;
    }
    mpp_buffer_put(buffer);

    MppBufferGroup group_to_put = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(in_flight_ > 0)
        {
            --in_flight_;
        }
        else{
            RKCAM_LOGE("[%s] releaseBuffer called but in_flight already 0",
                       config_.pool_name.c_str());
        }
        // RKCAM_LOGI("[%s] release buffer, in_flight=%d",
        //     config_.pool_name.c_str(),
        //     in_flight_);
        /*
         * 如果 close() 已经调用过，并且最后一个 buffer 也归还了，
         * 这里释放 group。
         */
        if (stopped_ && in_flight_ == 0 && group_) {
            group_to_put = group_;
            group_ = nullptr;
        }
    }
    if(group_to_put)
    {
        mpp_buffer_group_put(group_to_put);
        RKCAM_LOGI("[%s] group put after delayed close",
                   config_.pool_name.c_str());
    }

}

bool MppBufferPool::calcLayout()
{
    if (config_.width <= 0 || config_.height <= 0) {
        return false;
    }

    hor_stride_ = config_.hor_stride > 0
        ? config_.hor_stride
        : alignTo(config_.width, config_.stride_align);

    ver_stride_ = config_.ver_stride > 0
        ? config_.ver_stride
        : alignTo(config_.height, config_.height_align);

    if (hor_stride_ < config_.width ||
        ver_stride_ < config_.height) {
        RKCAM_LOGE("[%s] invalid stride: visible=%dx%d stride=%dx%d",
                   config_.pool_name.c_str(),
                   config_.width,
                   config_.height,
                   hor_stride_,
                   ver_stride_);
        return false;
    }

    frame_size_ = calcFrameSize(
        config_.format,
        hor_stride_,
        ver_stride_);

    if (frame_size_ == 0) {
        return false;
    }

    return true;
}
int MppBufferPool::alignTo(int value, int align)
{
    if (value <= 0 || align <= 0) {
        return 0;
    }

    return (value + align - 1) / align * align;
}
int MppBufferPool::calcPlaneStrideBytes(
    PixelFormat format,
    int hor_stride)
{
    if (hor_stride <= 0) {
        return 0;
    }

    switch (format) {
    case PixelFormat::NV12:
        /*
         * NV12 Y 行：
         *   1 pixel = 1 byte
         */
        return hor_stride;

    case PixelFormat::RGB888:
    case PixelFormat::BGR888:
        return hor_stride * 3;

    default:
        return 0;
    }
}
size_t MppBufferPool::calcFrameSize(
    PixelFormat format,
    int hor_stride,
    int ver_stride)
{
    if (hor_stride <= 0 || ver_stride <= 0) {
        return 0;
    }

    switch (format) {
    case PixelFormat::NV12:
        /*
         * RK MPP 编码输入 buffer size 通常按 64 对齐计算。
         *
         * 注意：
         *   hor_stride / ver_stride 仍然传 640 / 368；
         *   只是底层 buffer size 要留到 align64(ver_stride)=384。
         */
        return static_cast<size_t>(alignTo(hor_stride, 64)) *
               static_cast<size_t>(alignTo(ver_stride, 64)) *
               3 / 2;

    case PixelFormat::RGB888:
    case PixelFormat::BGR888:
        return static_cast<size_t>(alignTo(hor_stride, 64)) *
               static_cast<size_t>(alignTo(ver_stride, 64)) *
               3;

    default:
        return 0;
    }
}
}