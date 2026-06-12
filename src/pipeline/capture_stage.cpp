#include "rkcam/pipeline/capture_stage.hpp"
#include "rkcam/core/log.hpp"

#include <utility>

namespace rkcam{
CaptureStage::CaptureStage(
    const CaptureStageConfig& config,
    std::unique_ptr<IVideoSource> source,
    BlockingQueue<PipelineVideoFrame>& output_queue
): config_(config), source_(std::move(source)), output_queue_(output_queue)
{

}

CaptureStage::~CaptureStage()
{
    stop();
}

bool CaptureStage::start()
{
    if(running_){
        return true;
    }

    if(!source_->open())
    {
        RKCAM_LOGE("[%s] CaptureStage source open failed", config_.stream_id.c_str());
        return false;
    }

    if(!source_->start())
    {
        RKCAM_LOGE("[%s] CaptureStage source start failed",
                   config_.stream_id.c_str());
        source_->close();
        return false;
    }

    running_ = true;
    thread_ = std::thread(&CaptureStage::threadLoop, this);

    RKCAM_LOGI("[%s] CaptureStage started", config_.stream_id.c_str());
    return true;
}
void CaptureStage::stop()
{
    if(!running_)
    {
        return;
    }
    running_ = false;

        /*
     * 尝试让 readFrame 尽快返回。
     * 如果 readFrame 内部正在 select/poll，stop 后也可能还要等一次超时，
     * 但通常 STREAMOFF 可以帮助采集链路停止。
     */
    if (source_) {
        source_->stop();
    }


    if(thread_.joinable())
    {
        thread_.join();
    }
    source_->close();

     RKCAM_LOGI("[%s] CaptureStage stopped", config_.stream_id.c_str());

}


void CaptureStage::threadLoop()
{
    while(running_)
    {
        VideoSourceFrame src_frame;
        if(! source_->readFrame(src_frame))
        {
            continue;
        }
        bool ok = false;


        switch(config_.output_memory_type)
        {
            case VideoMemoryType::Cpu:
                ok = handleCpuFrame(src_frame);
                break;
            case VideoMemoryType::DmaBuffer:
                ok = handleDmaFrame(src_frame);
                break;
            default:
                RKCAM_LOGE("[%s] CaptureStage unsupported output memory type",
                       config_.stream_id.c_str());

                /*
                * 已经 DQBUF 的 frame 必须归还，否则 V4L2 buffer 会被耗尽。
                */
                source_->releaseFrame(src_frame);
                ok = false;
                break;
        };

        if(!ok){
            /*
             * 当前先不中断整个 stage。
             * 例如队列满丢帧、copy 失败，都可以继续采下一帧。
             * 真正严重错误在 handle 函数内部会设置 running_ = false。
             */
            continue;
        }

    }

}



bool CaptureStage::handleCpuFrame(const VideoSourceFrame& src)
{
    PipelineVideoFrame out_frame;

    bool copied = copyVideoFrameToCpuPipelineFrame(
        src,
        out_frame,
        config_.stream_id
    );

    if(!source_->releaseFrame(src))
    {
        RKCAM_LOGE("[%s] CaptureStage releaseFrame failed",
            config_.stream_id.c_str());
        running_ = false;
        return false;
    }

    if(!copied)
    {
        RKCAM_LOGE("[%s] CaptureStage copyVideoFrameToCpuPipelineFrame failed",
            config_.stream_id.c_str());
        return false;
    }
    if (!output_queue_.push(std::move(out_frame))) {
        /*
         * push 返回 false 的常见原因：
         * 1. 队列 stop 了
         * 2. QueueFullPolicy::DropNewest 下队列满，当前帧被丢弃
         *
         * 如果队列已经 stopped，说明 pipeline 正在退出，可以停止采集线程。
         */
        if (output_queue_.stopped()) {
            running_ = false;
        }

        return false;
    }
    return true;

}

bool CaptureStage::handleDmaFrame(const VideoSourceFrame& src)
{
    /*
     * 当前还没有实现 DMA/DMABUF 零拷贝。
     *
     * 后面实现时这里应该做：
     *   1. V4L2VideoSource 支持导出 dma_fd，或者 VideoFrame 已经携带 dma_fd
     *   2. 创建 PipelineVideoFrame
     *   3. frame.buffer->memory_type = VideoMemoryType::DmaBuffer
     *   4. frame.buffer->planes[p].dma_fd = ...
     *   5. frame.buffer->release_cb 负责最终归还 V4L2 buffer
     *
     * 注意：
     *   DMA 零拷贝模式不能像 CPU copy 一样马上 releaseFrame。
     *   必须等后续 RGA / MPP 使用完后再归还。
     */

    RKCAM_LOGE("[%s] CaptureStage DmaBuffer mode is not implemented yet",
               config_.stream_id.c_str());

    /*
     * 当前未实现 DMA，所以必须立即归还这个 V4L2 buffer，
     * 否则采几帧后 buffer 会耗尽。
     */
    if (!source_->releaseFrame(src)) {
        RKCAM_LOGE("[%s] CaptureStage releaseFrame failed in DmaBuffer placeholder",
                   config_.stream_id.c_str());
        running_ = false;
    }

    return false;
}
}