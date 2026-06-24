#include "rkcam/video/pipeline_video_frame.hpp"
#include "rkcam/core/log.hpp"

#include <cstring>
#include <memory>

namespace rkcam{

bool copyVideoFrameToCpuPipelineFrame(const VideoSourceFrame& src, 
                                PipelineVideoFrame& dst,
                                const std::string& stream_id){
    if (src.planes.empty()) {
        RKCAM_LOGE("src.planes is empty!");
        return false;
    }
    dst.stream_id = stream_id;
    dst.width = src.width;
    dst.height = src.height;
    dst.format = src.format;

    dst.pts_us = src.pts_us;
    dst.duration_us = src.duration_us;
    dst.frame_id = src.frame_id;

    std::shared_ptr<VideoBuffer> buffer = std::make_shared<VideoBuffer>();
    buffer->memory_type = VideoMemoryType::Cpu;
    buffer->planes.resize(src.planes.size());

    for(size_t i = 0; i < buffer->planes.size(); i++)
    {

        if(!src.planes[i].data || src.planes[i].bytesused == 0)
        {
            RKCAM_LOGE("Failed to copy frame! Stream: %s, FrameID: %lld, Plane: %zu is invalid. "
                       "Reason: [data_ptr = %p, bytesused = %zu]",
                       dst.stream_id.c_str(), 
                       static_cast<long long>(src.frame_id), 
                       i, 
                       src.planes[i].data, 
                       src.planes[i].bytesused);
            return false;
        }

        buffer->planes[i].cpu_storage.resize(src.planes[i].bytesused);
        std::memcpy(buffer->planes[i].cpu_storage.data(), src.planes[i].data, src.planes[i].bytesused);

        buffer->planes[i].data = buffer->planes[i].cpu_storage.data();
        buffer->planes[i].length = buffer->planes[i].cpu_storage.size();
        buffer->planes[i].bytesused = src.planes[i].bytesused;
        buffer->planes[i].dma_fd = -1;
        buffer->planes[i].offset = 0;
        buffer->planes[i].stride = src.planes[i].stride;
    }
    dst.buffer = buffer;
    return true;
}








}