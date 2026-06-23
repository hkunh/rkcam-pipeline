#pragma once

#include "rkcam/pipeline/pipeline_stage.hpp"
#include "rkcam/pipeline/pipeline_video_frame.hpp"
#include "rkcam/video/rga_processor.hpp"
#include "rkcam/core/blocking_queue.hpp"
#include "rkcam/pipeline/video_buffer_pool.hpp"
#include "rkcam/pipeline/mpp_buffer_pool.hpp"

#include <atomic>
#include <string>
#include <thread>


namespace rkcam{
enum class RgaOutputPoolType{
    None,
    Mpp,
};
struct RgaStageConfig{
    std::string stage_name = "rga";
    /*
     * RGA 处理请求。
     *
     * 当前第一版建议：
     *   input : DmaBuffer
     *   output: Cpu
     *
     * 例如：
     *   output_width = 640
     *   output_height = 360
     *   output_format = PixelFormat::NV12
     *   output_memory_type = VideoMemoryType::Cpu
     */
    RgaProcessRequest request;

    RgaOutputPoolType output_pool_type = RgaOutputPoolType::Mpp;

    MppBufferPoolConfig mpp_buffer_pool;

};
class RgaStage : public IStage{
public:
    RgaStage(
        const RgaStageConfig& config,
        BlockingQueue<PipelineVideoFrame>& input_queue,
        BlockingQueue<PipelineVideoFrame>& output_queue
    );
    ~RgaStage() override;
    RgaStage(const RgaStage&) = delete;
    RgaStage& operator=(const RgaStage&) = delete;

    bool start() override;
    void stop() override;
private:
    bool createOutputPool();
    void threadLoop();
    /*
     * 根据 request.output_memory_type 准备 output frame。
     *
     * Cpu:
     *   不准备 buffer，让 RgaProcessor 创建。
     *
     * DmaBuffer:
     *   从 output_dma_pool_ acquire 一个 VideoBuffer。
     */
    bool prepareOutputFrame(
        const PipelineVideoFrame& input,
        PipelineVideoFrame& output);
private:
    RgaStageConfig config_;

    BlockingQueue<PipelineVideoFrame>& input_queue_;
    BlockingQueue<PipelineVideoFrame>& output_queue_;

    RgaProcessor processor_;

    std::shared_ptr<IVideoBufferPool> output_pool_;

    std::thread thread_;
    std::atomic<bool> running_{false};

    int processed_frames_ = 0;
};



}