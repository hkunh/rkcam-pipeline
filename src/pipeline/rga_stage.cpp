#include "rkcam/pipeline/rga_stage.hpp"
#include "rkcam/core/log.hpp"

#include <utility>


namespace rkcam {

RgaStage::RgaStage(
    const RgaStageConfig& config,
    BlockingQueue<PipelineVideoFrame>& input_queue,
    BlockingQueue<PipelineVideoFrame>& output_queue)
    : config_(config),
      input_queue_(input_queue),
      output_queue_(output_queue)
{
}
RgaStage::~RgaStage()
{
    stop();
}


bool RgaStage::start()
{
    if(running_){
        return true;
    }


    running_ = true;
    processed_frames_ = 0;
    thread_ = std::thread(&RgaStage::threadLoop, this);
    RKCAM_LOGI("[%s] RgaStage started", config_.stage_name.c_str());

    return true;
}
void RgaStage::stop()
{
    running_ = false;

    if(thread_.joinable())
    {
        thread_.join();
    }
    RKCAM_LOGI("[%s] RgaStage stopped, processed_frames=%d",
            config_.stage_name.c_str(),
            processed_frames_);
}

void RgaStage::threadLoop()
{
    while(running_)
    {
        PipelineVideoFrame input;
        if(!input_queue_.pop(input))
        {
            if (input_queue_.stopped()) {
                break;
            }
            continue;
        }
        PipelineVideoFrame output;
        if(!processor_.process(input, output, config_.request))
        {
            RKCAM_LOGE("[%s] RGA process failed, stream=%s frame_id=%lld",
            config_.stage_name.c_str(),
            input.stream_id.c_str(),
            static_cast<long long>(input.frame_id));

            continue;
        }
        ++processed_frames_;
        if(!output_queue_.push(std::move(output)))
        {
            RKCAM_LOGE("[%s] output_queue push failed",
            config_.stage_name.c_str());
            if(output_queue_.stopped())
            {
                running_ = false;
                input_queue_.stop();
                input_queue_.clear();
                break;
            }
        }
        continue;

    }
    /*
     * RgaStage 是 processed_frame_queue_ 的生产者。
     * 退出时必须 stop 输出队列，通知 RawSaveStage EOF。
     */
    output_queue_.stop();

    RKCAM_LOGI("[%s] RgaStage thread exit, processed_frames=%d",
               config_.stage_name.c_str(),
               processed_frames_);
}

}