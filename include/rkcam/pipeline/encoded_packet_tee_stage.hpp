#pragma once

#include "rkcam/core/blocking_queue.hpp"
#include "rkcam/media/encoded_packet.hpp"
#include "rkcam/pipeline/pipeline_stage.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>


namespace rkcam{

struct EncodedPacketTeeStageConfig{

    std::string stage_name = "encoded_packet_tee";

    /*
     * 某个输出队列 push 失败时，是否继续推给其他输出队列。
     *
     * 录像 + 推流场景建议 true：
     *   推流断开，不应该影响本地录像；
     *   本地录像停止，也不应该影响推流。
     */
    bool continue_on_output_fail = true;

    /*
     * <= 0 表示不打印周期日志。
     */
    int log_interval = 30;
};

class EncodedPacketTeeStage : public IStage{
public:
    EncodedPacketTeeStage(const EncodedPacketTeeStageConfig& config,
                    BlockingQueue<EncodedPacket>& input_queue,
                    const std::vector<BlockingQueue<EncodedPacket>*>& output_queues);

    ~EncodedPacketTeeStage() override;

    EncodedPacketTeeStage(const EncodedPacketTeeStage&) = delete;
    EncodedPacketTeeStage& operator=(const EncodedPacketTeeStage&) = delete;

    bool start() override;
    void stop() override;

private:
    void threadLoop();
    void stopOutputQueues();

private:
    EncodedPacketTeeStageConfig config_;

    BlockingQueue<EncodedPacket>& input_queue_;
    std::vector<BlockingQueue<EncodedPacket>*> output_queues_;

    std::thread thread_;
    std::atomic<bool> running_{false};

    int input_packets_ = 0;
    int forwarded_packets_ = 0;
    int failed_pushes_ = 0;

};



}// namespace rkcam