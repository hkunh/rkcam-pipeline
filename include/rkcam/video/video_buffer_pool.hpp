#pragma once

#include "rkcam/video/video_frame.hpp"

#include <memory>

namespace rkcam {

class IVideoBufferPool {
public:
    virtual ~IVideoBufferPool() = default;

    virtual bool init() = 0;
    virtual void close() = 0;

    virtual std::shared_ptr<VideoBuffer> acquire() = 0;

    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual int horStride() const = 0;
    virtual int verStride() const = 0;
    virtual PixelFormat format() const = 0;
};

} // namespace rkcam