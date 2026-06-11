
#pragma once
#include "rkcam/video/video_source_frame.hpp"

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
     * 通用信息。
     */
    size_t length = 0;
    size_t bytesused = 0;
    int stride = 0;
};
struct VideoBuffer{
    VideoMemoryType memory_type = VideoMemoryType::Cpu;

    std::vector<VideoBufferPlane> planes;

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