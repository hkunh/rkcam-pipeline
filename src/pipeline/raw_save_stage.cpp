#include "rkcam/pipeline/raw_save_stage.hpp"
#include "rkcam/core/log.hpp"

#include <cstring>

namespace rkcam{

RawSaveStage::RawSaveStage(
    const RawSaveStageConfig& config,
    BlockingQueue<PipelineVideoFrame>& input_queue)
    : config_(config),
      input_queue_(input_queue)
{
}


RawSaveStage::~RawSaveStage()
{
    stop();
}

bool RawSaveStage::start()
{
    if(running_)
    {
        return true;
    }
    if(!openFile())
    {
        return false;
    }

    running_ = true;
    saved_frames_ = 0;
    thread_ =std::thread(&RawSaveStage::threadLoop, this);

    RKCAM_LOGI("[%s] RawSaveStage started, output=%s",
               config_.stage_name.c_str(),
               config_.output_path.c_str());

    return true;

}

void RawSaveStage::stop()
{
    bool was_running = running_;
    running_ = false;

    /*
     * 外部强制 stop 时，用于唤醒阻塞在 pop() 的线程。
     *
     * 正常流程中：
     *   上游 RgaStage / CaptureStage 会 stop 输出队列，
     *   RawSaveStage 会自然收到 EOF 后退出。
     *
     * 这里重复 stop 是安全的。
     */
    input_queue_.stop();

    if (thread_.joinable()) {
        thread_.join();
    }

    closeFile();

    RKCAM_LOGI("[%s] RawSaveStage stopped, saved_frames=%d",
                config_.stage_name.c_str(),
                saved_frames_);


}

bool RawSaveStage::openFile()
{
    fp_ = std::fopen(config_.output_path.c_str(), "wb");
    if(!fp_)
    {
        RKCAM_LOGE("[%s] failed to open output file: %s",
            config_.stage_name.c_str(),
            config_.output_path.c_str());
        return false;
    }
    return true;
}
void RawSaveStage::closeFile()
{
    if(fp_)
    {
        std::fflush(fp_);
        std::fclose(fp_);
        fp_ = nullptr;
    }
}

void RawSaveStage::threadLoop()
{
    while(running_){
        PipelineVideoFrame frame;
        if(!input_queue_.pop(frame))
        {
            /*
             * pop 返回 false 的核心含义：
             *   queue stopped 且 queue empty。
             *
             * 这就是上游发来的 EOF。
             */
            if (input_queue_.stopped()) {
                break;
            }

            continue;
        }
        if(!writeFrame(frame))
        {
            RKCAM_LOGE("[%s] writeFrame failed, stream=%s, frame_id=%lld",
                       config_.stage_name.c_str(),
                       frame.stream_id.c_str(),
                       static_cast<long long>(frame.frame_id));
            continue;
        }

        ++saved_frames_;
        if (saved_frames_ % 30 == 0) {
            RKCAM_LOGI("[%s] saved_frames=%d, stream=%s, frame_id=%lld",
                       config_.stage_name.c_str(),
                       saved_frames_,
                       frame.stream_id.c_str(),
                       static_cast<long long>(frame.frame_id));
        }
    }
    RKCAM_LOGI("[%s] RawSaveStage thread exit, saved_frames=%d",
            config_.stage_name.c_str(),
            saved_frames_);
}

bool RawSaveStage::writeFrame(const PipelineVideoFrame& frame)
{
    if(!fp_)
    {
        return false;
    }
    if(!frame.buffer)
    {
        RKCAM_LOGE("[%s] frame.buffer is null", config_.stage_name.c_str());
        return false;
    }
    /*
     * DmaBuffer 也可以保存，但前提是 plane.data != nullptr。
     *
     * 当前你的 CaptureStage 是 V4L2 MMAP + EXPBUF，
     * 所以 DmaBuffer 里可以带 borrowed mmap data。
     *
     * 如果后面是纯 dma_fd，没有 CPU 映射地址，
     * RawSaveStage 不能直接保存，需要先 RGA 转 CPU 或 mmap dma-buf。
     */
    if (frame.buffer->memory_type != VideoMemoryType::Cpu &&
        frame.buffer->memory_type != VideoMemoryType::DmaBuffer) {
        RKCAM_LOGE("[%s] unsupported memory type=%d",
                   config_.stage_name.c_str(),
                   static_cast<int>(frame.buffer->memory_type));
        return false;
    }

    if (frame.format == PixelFormat::NV12 && config_.save_tight_nv12) {
        return writeFrameTightNv12(frame);
    }

    return writeFrameRawLayout(frame);

}
bool RawSaveStage::writeFrameRawLayout(const PipelineVideoFrame& frame)
{
    const auto& planes = frame.buffer->planes;
    for(const auto& plane : planes)
    {
        if(!plane.data || plane.bytesused == 0)
        {
            return false;
        }
        size_t written = std::fwrite(plane.data, 1, plane.bytesused, fp_);
        if(written != plane.bytesused)
        {
            return false;
        }
    }
    return true;
}


bool RawSaveStage::writeFrameTightNv12(const PipelineVideoFrame& frame)
{
    if(frame.width <= 0 || frame.height <= 0)
    {
        RKCAM_LOGE("[%s] invalid NV12 size: %dx%d",
            config_.stage_name.c_str(),
            frame.width,
            frame.height);
        return false;
    }

    const auto& planes = frame.buffer->planes;
    if (planes.empty()) {
        RKCAM_LOGE("[%s] NV12 frame has no planes",
                   config_.stage_name.c_str());
        return false;
    }
    /*
     * 情况 1：
     *   单 plane NV12，plane[0] 中连续保存 Y + UV。
    */
    if(planes.size() == 1)
    {
        const auto& p0 = planes[0];
        if(!p0.data || p0.stride <= 0)
        {
            RKCAM_LOGE("[%s] NV12 plane0 data is null, memory=%d dma_fd=%d",
                       config_.stage_name.c_str(),
                       static_cast<int>(frame.buffer->memory_type),
                       p0.dma_fd);
            return false;
        }
        const int width = frame.width;
        const int height = frame.height;
        const int stride = p0.stride;

        const uint8_t* base = p0.data;

        // Y: height 行，每行写 width 字节
        for (int y = 0; y < height; ++y) {
            const uint8_t* row = base + static_cast<size_t>(y) * stride;
            if (std::fwrite(row, 1, width, fp_) != static_cast<size_t>(width)) {
                return false;
            }
        }

        // UV: height/2 行，每行写 width 字节
        const uint8_t* uv_base =
            base + static_cast<size_t>(stride) * height;

        for (int y = 0; y < height / 2; ++y) {
            const uint8_t* row = uv_base + static_cast<size_t>(y) * stride;
            if (std::fwrite(row, 1, width, fp_) != static_cast<size_t>(width)) {
                return false;
            }
        }
        return true;
    }

    /*
     * 情况 2：
     *   两 plane NV12：
     *   plane[0] = Y
     *   plane[1] = UV
     */
    if(planes.size() >= 2)
    {
        const auto& y_plane = planes[0];
        const auto& uv_plane = planes[1];

        if (!y_plane.data || !uv_plane.data ||
            y_plane.stride <= 0 || uv_plane.stride <= 0) {
            return false;
        }

        const int width = frame.width;
        const int height = frame.height;

        for(int y = 0; y <height; y++)
        {
            const uint8_t* row = y_plane.data + static_cast<size_t>(y) * y_plane.stride;
            if(std::fwrite(row, 1, width, fp_) != static_cast<size_t>(width)){
                return false;
            }
        }
        for(int y = 0; y < height / 2; y++)
        {
            const uint8_t* row = uv_plane.data + static_cast<size_t>(y) * uv_plane.stride;
            if(std::fwrite(row, 1, width, fp_) != static_cast<size_t>(width)){
                return false;
            }
        }
        return true;

    }
    return true;
}


}