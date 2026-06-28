#pragma once

#include "rkcam/core/blocking_queue.hpp"
#include "rkcam/media/encoded_packet.hpp"
#include "rkcam/pipeline/pipeline_stage.hpp"
#include "rkcam/video/video_frame.hpp"
#include "rkcam/platform/rockchip/mpp/mpp_encoder.hpp"

#include <atomic>
#include <thread>
#include <string>

namespace rkcam{

struct MppStageConfig{
    std::string stage_name = "mpp";

    /*
    * MPP 编码器配置。
    *
    * 注意：
    *   encoder.width / height 必须和输入到 MppStage 的 frame.width / height 一致。
    *   也就是 RgaStage 的输出尺寸。
    */
    MppEncoderConfig encoder;

};
class MppStage : public IStage{
public:
    MppStage(const MppStageConfig& config, BlockingQueue<PipelineVideoFrame>& input_queue, BlockingQueue<EncodedPacket>& output_queue);
    ~MppStage() override;

    MppStage(const MppStage&) = delete;
    MppStage& operator=(const MppStage&) = delete;

    bool start() override;
    void stop() override;

private:
    void threadLoop();

private:
    MppStageConfig config_;

    BlockingQueue<PipelineVideoFrame>& input_queue_;
    BlockingQueue<EncodedPacket>& output_queue_;

    std::thread thread_;

    MppEncoder encoder_;

    std::atomic<bool> running_{false};

    int encoded_packets_ = 0;
    int failed_frames_ = 0;



};


}