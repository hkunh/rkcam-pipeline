
#pragma once
#include "rkcam/video/video_source.hpp"

#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rkcam {


enum class VideoMemoryType{
    Cpu,
    DmaBuffer,
};
struct NativeBufferHandle {
    virtual ~NativeBufferHandle() = default;
};
struct VideoBufferPlane{

    /*
     * CPU 模式：
     *   data 指向 cpu_storage.data()
     *
     * DMA 模式：
     *   data 可以为空，主要使用 dma_fd
     */
    uint8_t* data = nullptr;

    /*
     * CPU 模式下真正拥有的数据。
     * DMA 模式下通常为空。
     */
    std::vector<uint8_t> cpu_storage;

    /*
     * DMA 模式使用。
     */
    int dma_fd = -1;
    size_t offset = 0;

    /*
     * length:
     *   这个 memory plane 实际分配大小。
     *
     * bytesused:
     *   当前有效使用大小。
     *
     * stride:
     *   bytes per line。
     *
     * height_stride:
     *   这个 memory plane 对应的实际分配行数。
     *
     * 对 NV12 single-plane：
     *   stride = Y 行字节数
     *   height_stride = Y 的对齐行数
     *   UV 起始 = data + stride * height_stride
     *
     * 对 NV12M：
     *   plane[0].height_stride = Y 对齐行数
     *   plane[1].height_stride = UV 对齐行数
     */
    size_t length = 0;
    size_t bytesused = 0;
    int stride = 0;
    int height_stride = 0;
};
struct VideoBuffer{
    VideoMemoryType memory_type = VideoMemoryType::Cpu;

    std::vector<VideoBufferPlane> planes;


    /*
     * 用于保存 MppBuffer / DRM handle / 其他平台 native handle。
     * 上层普通 stage 不需要关心它。
     */
    std::shared_ptr<NativeBufferHandle> native_handle;

    /*
     * 可选释放回调。
     *
     * CPU 模式：
     *   一般不需要，vector 自动释放。
     *
     * DMA / V4L2 零拷贝模式：
     *   后面可以在这里放 releaseFrame / close(fd) 等逻辑。
     */
    std::function<void()> release_cb;

    ~VideoBuffer(){
        if(release_cb){
            release_cb();
        }
    }

    VideoBuffer(const VideoBuffer&) = delete;
    VideoBuffer& operator=(const VideoBuffer&) = delete;

    VideoBuffer() = default;


};
struct PipelineVideoFrame{
    std::string stream_id = "cam0";

    int width = 0;
    int height = 0;

    PixelFormat format = PixelFormat::Unknown;

    int64_t pts_us = 0;
    int64_t duration_us = 0;
    int64_t frame_id = 0;

    /*
     * 真正的数据内存。
     * 用 shared_ptr 的好处：
     *   1. 队列传递 frame 时不会复制大块图像数据
     *   2. 多个 stage 可以共享同一块 buffer
     *   3. 最后一个引用释放时，可以自动触发 release_cb
     */
    std::shared_ptr<VideoBuffer> buffer;
};
bool copyVideoFrameToCpuPipelineFrame(const VideoSourceFrame& src, PipelineVideoFrame& dst, const std::string& stream_id);
}