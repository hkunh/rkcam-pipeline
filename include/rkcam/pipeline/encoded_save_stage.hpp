#pragma once

#include "rkcam/core/blocking_queue.hpp"
#include "rkcam/media/encoded_packet.hpp"
#include "rkcam/pipeline/pipeline_stage.hpp"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

namespace rkcam {

struct EncodedSaveStageConfig {
    std::string stage_name = "encoded_save";
    std::string output_path;

    int max_packets = 0;
};

class EncodedSaveStage : public IStage {
public:
    EncodedSaveStage(
        const EncodedSaveStageConfig& config,
        BlockingQueue<EncodedPacket>& input_queue);

    ~EncodedSaveStage() override;

    EncodedSaveStage(const EncodedSaveStage&) = delete;
    EncodedSaveStage& operator=(const EncodedSaveStage&) = delete;

    bool start() override;
    void stop() override;

private:
    bool openFile();
    void closeFile();

    void threadLoop();
    bool writePacket(const EncodedPacket& packet);

private:
    EncodedSaveStageConfig config_;

    BlockingQueue<EncodedPacket>& input_queue_;

    std::thread thread_;
    std::atomic<bool> running_{false};

    FILE* fp_ = nullptr;
    int saved_packets_ = 0;
};

} // namespace rkcam