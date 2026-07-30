#pragma once

#include "rkcam/core/blocking_queue.hpp"
#include "rkcam/media/encoded_packet.hpp"
#include "rkcam/pipeline/pipeline_stage.hpp"

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <string>
#include <thread>

namespace rkcam {

struct AacAdtsSaveStageConfig{
    std::string stage_name = "aac_adts_save";
    /*
     * 输出 ADTS AAC 文件。
     * 例如：
     *   /userdata/rkcam/output/test_audio.aac
     */
    std::string output_path;

    /*
     * 必须和 AacEncoder 输出一致。
     *
     * 你当前建议：
     *   sample_rate = 48000
     *   channels    = 1
     */
    int sample_rate = 48000;
    int channels = 1;

    /*
     * <= 0 表示不限制。
     */
    int max_packets = 0;

    int log_interval = 50;

    /*
     * true:
     *   遇到非 AAC audio packet 直接报错计数。
     *
     * false:
     *   非 AAC audio packet 直接跳过。
     */
    bool strict_packet = true;
};

class AacAdtsSaveStage : public IStage{
public:
    AacAdtsSaveStage(const AacAdtsSaveStageConfig& config, BlockingQueue<EncodedPacket>& input_queue);

    ~AacAdtsSaveStage() override;

    AacAdtsSaveStage(const AacAdtsSaveStage&) = delete;
    AacAdtsSaveStage& operator=(const AacAdtsSaveStage&) = delete;

    bool start() override;
    void stop() override;

private:
    void threadLoop();

    bool openFile();
    void closeFile();

    bool validatePacket(const EncodedPacket& packet) const;
    bool writePacket(const EncodedPacket& packet);

    bool makeAdtsHeader(size_t payload_size, uint8_t header[7]) const;

    static int aacSampleRateIndex(int sample_rate);
private:
    AacAdtsSaveStageConfig config_;
    BlockingQueue<EncodedPacket>& input_queue_;

    std::thread thread_;
    std::atomic<bool> running_{false};

    std::FILE* fp_ = nullptr;
    int saved_packets_ = 0;
    int failed_packets_ = 0;
    uint64_t written_bytes_ = 0;
    


};




}