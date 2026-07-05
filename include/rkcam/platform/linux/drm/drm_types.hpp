#pragma once
#include "rkcam/video/video_types.hpp"

#include <cstddef>
#include <cstdint>

namespace rkcam {

struct DrmPlaneDesc{
    int dma_fd = -1;
    uint32_t offset = 0;
    uint32_t pitch = 0;
};

struct DrmFrameDesc{
    int width = 0;
    int height = 0;
    PixelFormat format = PixelFormat::Unknown;

    /*
    * NV12:
    *   planes[0] = Y
    *   planes[1] = UV
    */
    DrmPlaneDesc planes[4];
    int plane_count = 0;
};

struct DrmDisplayRect{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};


}