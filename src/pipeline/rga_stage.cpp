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

    if (!createOutputPool()) {
        RKCAM_LOGE("[%s] createOutputPool failed",
                   config_.stage_name.c_str());
        return false;
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
    /*
     * 强制退出时唤醒阻塞在 input_queue_.pop() 的线程。
     *
     * 正常 EOF 路径下，CaptureStage 已经 stop raw_queue。
     * 这里重复 stop 是安全的。
     */
    input_queue_.stop();
    if(thread_.joinable())
    {
        thread_.join();
    }

    /*
     * RgaStage 是 output_queue_ 的生产者。
     * 停止时必须通知下游 EOF。
     */
    output_queue_.stop();


    /*
     * 这里 reset 是安全的：
     *   acquire() 返回的 VideoBuffer::release_cb 捕获了 shared_ptr<DmaBufferPool>，
     *   所以下游还持有 DMA buffer 时，pool 不会提前析构。
     */
    output_pool_.reset();
    RKCAM_LOGI("[%s] RgaStage stopped, processed_frames=%d",
            config_.stage_name.c_str(),
            processed_frames_);
}
bool RgaStage::createOutputPool()
{
    output_pool_.reset();
    if(config_.request.output_memory_type == VideoMemoryType::Cpu)
    {
        return true;
    }


    switch(config_.output_pool_type)
    {
        case RgaOutputPoolType::Mpp: {
            auto pool = std::make_shared<MppBufferPool>(config_.mpp_buffer_pool);
            if(!pool->init())
            {
                return false;
            }
            output_pool_ = pool;
            RKCAM_LOGI("[%s] use MppBufferPool: %dx%d stride=%dx%d format=%d",
                   config_.stage_name.c_str(),
                   output_pool_->width(),
                   output_pool_->height(),
                   output_pool_->horStride(),
                   output_pool_->verStride(),
                   static_cast<int>(output_pool_->format()));
            return true;
        }
        default:
            RKCAM_LOGE("[%s] DmaBuffer output requires output pool",
                   config_.stage_name.c_str());
            return false;
    }

}

bool RgaStage::prepareOutputFrame(
    const PipelineVideoFrame& input,
    PipelineVideoFrame& output)
{
    output = PipelineVideoFrame{};

    /*
     * CPU 输出：
     *   当前沿用你的已有逻辑，
     *   让 RgaProcessor 内部 createCpuOutputFrame()。
     */
    if (config_.request.output_memory_type == VideoMemoryType::Cpu) {
        return true;
    }

    /*
     * DMA 输出：
     *   RgaStage 必须提前准备 output.buffer。
     */
    if (config_.request.output_memory_type != VideoMemoryType::DmaBuffer) {
        RKCAM_LOGE("[%s] unsupported output memory type=%d",
                   config_.stage_name.c_str(),
                   static_cast<int>(config_.request.output_memory_type));
        return false;
    }

    if (!output_pool_) {
        RKCAM_LOGE("[%s] output_dma_pool_ is null",
                   config_.stage_name.c_str());
        return false;
    }

    auto out_buffer = output_pool_->acquire();
    if (!out_buffer) {
        RKCAM_LOGE("[%s] acquire output DMA buffer failed",
                   config_.stage_name.c_str());
        return false;
    }

    int out_width = config_.request.output_width;
    int out_height = config_.request.output_height;
    PixelFormat out_format = config_.request.output_format;

    /*
     * 如果 request 没设置输出尺寸，则默认继承输入。
     * 但 DMA pool 的 width/height 最好还是显式配置。
     */
    if (out_width <= 0) {
        out_width = input.width;
    }

    if (out_height <= 0) {
        out_height = input.height;
    }

    if (out_format == PixelFormat::Unknown) {
        out_format = input.format;
    }

    output.stream_id = input.stream_id;
    output.width = out_width;
    output.height = out_height;
    output.format = out_format;

    output.pts_us = input.pts_us;
    output.duration_us = input.duration_us;
    output.frame_id = input.frame_id;

    output.buffer = std::move(out_buffer);

    return true;
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


        if (!prepareOutputFrame(input, output)) {
            RKCAM_LOGE("[%s] prepareOutputFrame failed, stream=%s frame_id=%lld",
                       config_.stage_name.c_str(),
                       input.stream_id.c_str(),
                       static_cast<long long>(input.frame_id));
            continue;
        }


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