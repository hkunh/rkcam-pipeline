#include "rkcam/pipeline/pipeline_stage.hpp"
#include "rkcam/pipeline/pipeline_video_frame.hpp"
#include "rkcam/video/v4l2_video_source.hpp"
#include "rkcam/core/blocking_queue.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace rkcam{

struct CaptureStageConfig{
    std::string stream_id = "cam0";
    V4L2VideoSourceConfig source;
};

class CaptureStage : public IStage{

// public:
//     CaptureStage(
//         const CaptureStageConfig& config,

//     )

};


}