#pragma once
#include "rkcam/video/video_frame.hpp"

#include <string>
namespace rkcam{

class IVideoFrameProcessor{
public:
    virtual ~IVideoFrameProcessor() = default;

    virtual bool process(const PipelineVideoFrame& input, PipelineVideoFrame& outptu) = 0;
};

}
