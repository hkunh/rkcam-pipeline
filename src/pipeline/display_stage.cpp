#include "rkcam/pipeline/display_stage.hpp"

#include "rkcam/core/log.hpp"
#include <time.h>
namespace rkcam {

namespace{
int64_t nowUs()
{
    timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return static_cast<int64_t>(ts.tv_sec) * 1000000LL +
           static_cast<int64_t>(ts.tv_nsec) / 1000LL;
}
}

DisplayStage::DisplayStage(
    const DisplayStageConfig& config,
    BlockingQueue<PipelineVideoFrame>& input_queue,
    std::unique_ptr<IDisplaySink> sink)
    : config_(config),
      input_queue_(input_queue),
      sink_(std::move(sink))
{
}

DisplayStage::~DisplayStage()
{
    stop();
}

bool DisplayStage::start()
{
    if (running_) {
        return true;
    }

    if (!sink_) {
        RKCAM_LOGE("[%s] DisplayStage start failed: sink is null",
                   config_.stage_name.c_str());
        return false;
    }

    if (!sink_->open()) {
        RKCAM_LOGE("[%s] DisplayStage start failed: sink open failed",
                   config_.stage_name.c_str());
        return false;
    }

    displayed_frames_ = 0;
    failed_frames_ = 0;

    running_ = true;
    thread_ = std::thread(&DisplayStage::threadLoop, this);

    RKCAM_LOGI("[%s] DisplayStage started: display=%dx%d fmt=%d",
               config_.stage_name.c_str(),
               sink_->width(),
               sink_->height(),
               static_cast<int>(sink_->format()));

    return true;
}

void DisplayStage::stop()
{
    if (!running_ && !thread_.joinable()) {
        return;
    }

    input_queue_.stop();
    if(thread_.joinable())
    {
        thread_.join();
    }

    if(sink_)
    {
        sink_->close();
    }

    running_ = false;
    RKCAM_LOGI("[%s] DisplayStage stopped, displayed_frames=%d failed_frames=%d",
               config_.stage_name.c_str(),
               displayed_frames_,
               failed_frames_);
}


void DisplayStage::threadLoop()
{
    while(true)
    {
        PipelineVideoFrame frame;
        if(!input_queue_.pop(frame))
        {
            if(input_queue_.stopped())
            {
                break;
            }
            continue;
        }

        if(!sink_->displayFrame(frame))
        {
            ++failed_frames_;

            RKCAM_LOGE("[%s] displayFrame failed, stream=%s frame_id=%lld failed=%d",
                       config_.stage_name.c_str(),
                       frame.stream_id.c_str(),
                       static_cast<long long>(frame.frame_id),
                       failed_frames_);

            if (config_.max_failed_frames > 0 &&
                failed_frames_ >= config_.max_failed_frames) {
                RKCAM_LOGE("[%s] too many display failures, exit thread",
                           config_.stage_name.c_str());
                break;
            }

            continue;
        }
        ++displayed_frames_;
        const int64_t latency_us = nowUs() - frame.pts_us;
        if (displayed_frames_ % 30 == 0) {
            RKCAM_LOGI("[%s] displayed_frames=%d stream=%s frame_id=%lld latency_us=%lld",
                       config_.stage_name.c_str(),
                       displayed_frames_,
                       frame.stream_id.c_str(),
                       static_cast<long long>(frame.frame_id),
                        static_cast<long long>(latency_us));
        }
    }
    running_ = false;

    RKCAM_LOGI("[%s] DisplayStage thread exit, displayed_frames=%d failed_frames=%d",
               config_.stage_name.c_str(),
               displayed_frames_,
               failed_frames_);
}

}