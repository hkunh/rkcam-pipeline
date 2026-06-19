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
    captured_frames_ = 0;
    running_ = true;
    thread_ = std::thread(&CaptureStage::threadLoop, this);

    RKCAM_LOGI("[%s] CaptureStage started", config_.stream_id.c_str());
    return true;
}
void CaptureStage::stop()
{
    running_ = false;
    output_queue_.stop(); //可以重复，保险起见
        /*
     * 尝试让 readFrame 尽快返回。
     * 如果 readFrame 内部正在 select/poll，stop 后也可能还要等一次超时，
     * 但通常 STREAMOFF 可以帮助采集链路停止。
     */
    



    if(thread_.joinable())
    {
        thread_.join();
    }


    if(config_.output_memory_type == VideoMemoryType::DmaBuffer)
    {
        std::unique_lock<std::mutex> lock(dma_mutex_);
        dma_cv_.wait(lock, [this](){
            return dma_frames_in_flight_ == 0;
        });
    }

    /*
     * 所有 DMA frame 都归还后，再 STREAMOFF。
     */
    if (source_) {
        source_->stop();
        source_->close();
    }

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
        ++captured_frames_;
        if (captured_frames_ % 30 == 0 ||
            (config_.max_frames > 0 && captured_frames_ == config_.max_frames)) {
            RKCAM_LOGI("[%s] captured_frames=%d",
                       config_.stream_id.c_str(),
                       captured_frames_);
        }

        if (config_.max_frames > 0 &&
            captured_frames_ >= config_.max_frames) {
            RKCAM_LOGI("[%s] CaptureStage reached max_frames=%d",
                       config_.stream_id.c_str(),
                       config_.max_frames);

            running_ = false;
            break;
        }

    }
    /*
     * 正常结束时发 EOF。
     * 注意：这里只 stop，不 clear。
     * 下游还要把队列里剩余帧处理完。
     */
    output_queue_.stop();

    RKCAM_LOGI("[%s] CaptureStage thread exit, captured_frames=%d",
               config_.stream_id.c_str(),
               captured_frames_);

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
        RKCAM_LOGE("[%s] CaptureStage::handleCpuFrame output_queue_ stop",
            config_.stream_id.c_str());
        return false;
    }
    return true;

}

bool CaptureStage::handleDmaFrame(const VideoSourceFrame& src)
{
    if (src.width <= 0 || src.height <= 0) {
        RKCAM_LOGE("[%s] CaptureStage handleDmaFrame invalid size: %dx%d",
                   config_.stream_id.c_str(),
                   src.width,
                   src.height);

        source_->releaseFrame(src);
        return false;
    }

    if (src.format == PixelFormat::Unknown) {
        RKCAM_LOGE("[%s] CaptureStage handleDmaFrame unknown pixel format",
                   config_.stream_id.c_str());

        source_->releaseFrame(src);
        return false;
    }

    if (src.planes.empty()) {
        RKCAM_LOGE("[%s] CaptureStage handleDmaFrame src.planes is empty",
                   config_.stream_id.c_str());

        source_->releaseFrame(src);
        return false;
    }


    /*
     * 这里要求 V4L2VideoSource 已经 export_dma_fd = true。
     * 否则 dma_fd 会是 -1。
     */
    PipelineVideoFrame out_frame;
    out_frame.stream_id = config_.stream_id;
    out_frame.width = src.width;
    out_frame.height = src.height;
    out_frame.format = src.format;
    out_frame.pts_us = src.pts_us;
    out_frame.duration_us = src.duration_us;
    out_frame.frame_id = src.frame_id;

    auto buffer = std::make_shared<VideoBuffer>();
    buffer->memory_type = VideoMemoryType::DmaBuffer;
    buffer->planes.resize(src.planes.size());
    for(size_t i = 0; i < src.planes.size(); i++)
    {
        const auto& sp = src.planes[i];
        if(sp.dma_fd < 0)
        {
            RKCAM_LOGE("[%s] CaptureStage handleDmaFrame plane[%zu] invalid dma_fd=%d. "
            "Did you set V4L2VideoSourceConfig::export_dma_fd=true?",
            config_.stream_id.c_str(),
            i,
            sp.dma_fd);

            source_->releaseFrame(src);
            return false;
        }
        auto& dp = buffer->planes[i];
        dp.data = static_cast<uint8_t*>(sp.data);  // 借用 V4L2 mmap 地址，仅在 release_cb 触发前有效
        dp.cpu_storage.clear();

        dp.dma_fd = sp.dma_fd;
        dp.offset = 0;
        dp.length = sp.length;
        dp.bytesused = sp.bytesused;
        dp.stride = sp.stride;
    }
    {
        std::lock_guard<std::mutex> lock(dma_mutex_);
        ++dma_frames_in_flight_;
    }
    /*
     * 关键：
     *   不能在 handleDmaFrame() 里马上 releaseFrame(src)。
     *   要等 out_frame 被下游全部释放后，再由 release_cb 归还。
     *
     * 注意：
     *   这里捕获 this 是第一版可用写法。
     *   前提是 CaptureStage::stop() 会等待 dma_frames_in_flight_ 归零后再析构/close source。
     */
    buffer->release_cb = [this, src](){
        bool release_ok = true;
        if(source_){
            release_ok = source_->releaseFrame(src);
        }
        else{
            release_ok = false;
        }
        if (!release_ok) {
            RKCAM_LOGE("[%s] CaptureStage DMA release_cb releaseFrame failed, frame_id=%lld buffer_index=%d",
                       config_.stream_id.c_str(),
                       static_cast<long long>(src.frame_id),
                       src.buffer_index);
        }
        {
            std::lock_guard<std::mutex> lock(dma_mutex_);
            if(dma_frames_in_flight_ > 0)
            {
                --dma_frames_in_flight_;
            }
        }
        dma_cv_.notify_all();
    };
    out_frame.buffer = buffer;

    if(!output_queue_.push(std::move(out_frame)))
    {
        if (output_queue_.stopped()) {
            running_ = false;

        }
        RKCAM_LOGI("[%s] CaptureStage::handleDmaFrame output_queue_ stop",
            config_.stream_id.c_str());
        return false;
    }

    return true;
}
}