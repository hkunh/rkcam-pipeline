#pragma once
#include "rkcam/pipeline/pipeline_video_frame.hpp"

#include <string>
namespace rkcam{
enum class VideoFrameProcessType{
    Resize,
    Convert,
    
};
struct VideoRect{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};
struct VideoFrameProcessConfig{
    VideoFrameProcessType type = VideoFrameProcessType::Resize;


    /*
     * Crop / ROI 使用。
     * 如果 crop.width <= 0 或 crop.height <= 0，表示使用整张图。
     */
    VideoRect crop;


    int output_width = 0;
    int output_height = 0;

    PixelFormat output_format = PixelFormat::NV12;

    /*
     * 输出内存类型：
     *   Cpu       : 输出 CPU buffer，方便保存和调试
     *   DmaBuffer : 输出 DMA buffer，后面接 MPP 零拷贝
     */
    VideoMemoryType output_memory_type = VideoMemoryType::Cpu;
};

class IVideoFrameProcessor{
public:
    virtual ~IVideoFrameProcessor() = default;

    virtual bool process(const PipelineVideoFrame& input, PipelineVideoFrame& outptu) = 0;
};

}
