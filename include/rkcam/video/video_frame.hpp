/**
*@file include/rkcam/video/video_frame.hpp
*@brief 
*@author HKH368
*@date 2026-05-29
*@return 
*/

#pragma once

#include <cstdint>
#include <cstddef>

namespace rkcam {

enum class PixelFormat {
    Unknown,
    NV12,
    NV21,
    YUYV,
    RGB888,
    BGR888,
    RAW10,
    RAW12,
};

struct VideoFrame {
    int width = 0;
    int height = 0;
    int stride = 0;

    PixelFormat format = PixelFormat::Unknown;

    int64_t pts_us = 0;
    int64_t duration_us = 0;
    int64_t frame_id = 0;

    void* data = nullptr;
    size_t size = 0;

    int dma_fd = -1;
};

}
