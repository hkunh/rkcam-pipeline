#pragma once

#include "rkcam/audio/audio_frame.hpp"
#include "rkcam/core/blocking_queue.hpp"
#include "rkcam/media/aac_encoder.hpp"
#include "rkcam/media/encoded_packet.hpp"
#include "rkcam/pipeline/pipeline_stage.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace rkcam {

struct AacEncodeStageConfig{
    std::string stage_name = "aac_encode";
    std::string stream_id = "audio0";
    AacEncoderConfig encoder;

    /*
     * <= 0 表示不限制。
     */
    int max_frames = 0;

    int log_interval = 50;

    /*
     * 连续或累计编码失败过多时退出。
     * <= 0 表示不限制。
     */
    int max_encode_failures = 20;

};

class AacEncodeStage : public IStage{

public:
    AacEncodeStage(const AacEncodeStageConfig& config, 
        BlockingQueue<PipelineAudioFrame>& input_queue,
        BlockingQueue<EncodedPacket>& output_queue);

    ~AacEncodeStage() override;
    
    AacEncodeStage(const AacEncodeStage&) = delete;
    AacEncodeStage& operator=(const AacEncodeStage&) = delete;

    bool start() override;
    void stop() override;

private:
    void threadLoop();
    bool encodeOneFrame(const PipelineAudioFrame& frame);
    bool pushPackets(std::vector<EncodedPacket>& packets);
    bool flushEncoder();

    void normalizePacketMetadata(EncodedPacket& packet);

private:
    AacEncodeStageConfig config_;

    BlockingQueue<PipelineAudioFrame>& input_queue_;
    BlockingQueue<EncodedPacket>& output_queue_;

    std::unique_ptr<AacEncoder> encoder_;

    std::thread thread_;

    std::atomic<bool> running_{false};

    int input_frames_ = 0;
    int encoded_packets_ = 0;
    int failed_frames_ = 0;
    int failed_pushes_ = 0;

};

}