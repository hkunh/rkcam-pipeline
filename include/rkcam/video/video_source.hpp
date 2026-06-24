/**
*@file include/rkcam/video/video_source.hpp
*@brief 
*@author HKH368
*@date 2026-05-29
*@return 
*/


#pragma once
#include "rkcam/video/video_types.hpp"

#include <vector>
#include <cstdint>
#include <cstddef>

namespace rkcam{


struct VideoPlane{
    void* data = nullptr;
    size_t length = 0;
    size_t bytesused = 0;
    int dma_fd = -1;
    int stride = 0; 
};

struct VideoSourceFrame {
    int width = 0;
    int height = 0;

    PixelFormat format = PixelFormat::Unknown;

    int64_t pts_us = 0;
    int64_t duration_us = 0;
    int64_t frame_id = 0;

    std::vector<VideoPlane> planes;


    // V4L2 MMAP buffer 生命周期管理用
    int buffer_index = -1;
};


class IVideoSource{
public:
	virtual ~IVideoSource() = default;
	virtual bool open() = 0;
	virtual bool start() = 0;
	virtual bool readFrame(VideoSourceFrame& frame) = 0;
	virtual bool releaseFrame(const VideoSourceFrame& frame) = 0;
	virtual bool stop() = 0;
	virtual bool close() = 0;
};
}
