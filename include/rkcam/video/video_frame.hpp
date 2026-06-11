/**
*@file include/rkcam/video/video_frame.hpp
*@brief 
*@author HKH368
*@date 2026-05-29
*@return 
*/

#pragma once


#include <vector>
#include <cstdint>
#include <cstddef>

namespace rkcam {


#define VIDEO_MAX_PLANES 8

enum class PixelFormat {
    Unknown,
    NV12,
};


struct VideoPlane{
    void* data = nullptr;
    size_t length = 0;
    size_t bytesused = 0;
    int dma_fd = -1;
    int stride = 0; 
};

struct VideoFrame {
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

}
