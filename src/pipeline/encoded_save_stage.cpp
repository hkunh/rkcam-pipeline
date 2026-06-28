#include "rkcam/pipeline/encoded_save_stage.hpp"
#include "rkcam/core/log.hpp"
#include <time.h>
namespace rkcam {

EncodedSaveStage::EncodedSaveStage(
    const EncodedSaveStageConfig& config,
    BlockingQueue<EncodedPacket>& input_queue)
    : config_(config),
      input_queue_(input_queue)
{
}

EncodedSaveStage::~EncodedSaveStage()
{
    stop();
}

bool EncodedSaveStage::start()
{
    if (running_) {
        return true;
    }

    if (!openFile()) {
        return false;
    }

    saved_packets_ = 0;
    running_ = true;

    thread_ = std::thread(&EncodedSaveStage::threadLoop, this);

    RKCAM_LOGI("[%s] EncodedSaveStage started, output=%s",
               config_.stage_name.c_str(),
               config_.output_path.c_str());

    return true;
}

void EncodedSaveStage::stop()
{
    if (!running_ && !thread_.joinable()) {
        return;
    }

    /*
     * drain stop:
     * input_queue_.stop() 不会丢弃已有 EncodedPacket。
     * pop() 会继续取残留 packet，直到队列 empty。
     */
    input_queue_.stop();

    if (thread_.joinable()) {
        thread_.join();
    }

    closeFile();

    running_ = false;

    RKCAM_LOGI("[%s] EncodedSaveStage stopped, saved_packets=%d",
               config_.stage_name.c_str(),
               saved_packets_);
}

bool EncodedSaveStage::openFile()
{
    if (config_.output_path.empty()) {
        RKCAM_LOGE("[%s] output_path is empty",
                   config_.stage_name.c_str());
        return false;
    }

    fp_ = std::fopen(config_.output_path.c_str(), "wb");
    if (!fp_) {
        RKCAM_LOGE("[%s] failed to open output file: %s",
                   config_.stage_name.c_str(),
                   config_.output_path.c_str());
        return false;
    }

    return true;
}

void EncodedSaveStage::closeFile()
{
    if (fp_) {
        std::fflush(fp_);
        std::fclose(fp_);
        fp_ = nullptr;
    }
}

bool EncodedSaveStage::writePacket(const EncodedPacket& packet)
{
    if (!fp_) {
        return false;
    }

    if (packet.data.empty()) {
        return true;
    }

    const size_t written =
        std::fwrite(packet.data.data(), 1, packet.data.size(), fp_);

    return written == packet.data.size();
}

static int64_t nowUs()
{
    timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return static_cast<int64_t>(ts.tv_sec) * 1000000LL +
           static_cast<int64_t>(ts.tv_nsec) / 1000LL;
}

void EncodedSaveStage::threadLoop()
{
    while (true) {
        EncodedPacket packet;

        if (!input_queue_.pop(packet)) {
            if (input_queue_.stopped()) {
                break;
            }

            continue;
        }

        if (!writePacket(packet)) {
            RKCAM_LOGE("[%s] writePacket failed, stream=%s pts=%lld size=%zu",
                       config_.stage_name.c_str(),
                       packet.stream_id.c_str(),
                       static_cast<long long>(packet.pts_us),
                       packet.data.size());
            continue;
        }

        ++saved_packets_;

        // 2. ✨在这里计算端到端延迟：当前系统时间 - 视频帧诞生时的 pts 时间戳
        const int64_t latency_us = nowUs() - packet.pts_us;

        if (saved_packets_ % 30 == 0) {
            // 3. ✨修改打印日志，增加 latency=%.2f ms，并将 latency_us 转换为毫秒（除以 1000.0）
            RKCAM_LOGI("[%s] saved_packets=%d stream=%s pts=%lld size=%zu key=%d latency=%.2f ms",
                       config_.stage_name.c_str(),
                       saved_packets_,
                       packet.stream_id.c_str(),
                       static_cast<long long>(packet.pts_us),
                       packet.data.size(),
                       packet.key_frame ? 1 : 0,
                       latency_us / 1000.0); // 转换为毫秒输出
        }

        if (config_.max_packets > 0 &&
            saved_packets_ >= config_.max_packets) {
            RKCAM_LOGI("[%s] reached max_packets=%d",
                       config_.stage_name.c_str(),
                       config_.max_packets);
            break;
        }
    }

    running_ = false;

    RKCAM_LOGI("[%s] EncodedSaveStage thread exit, saved_packets=%d",
               config_.stage_name.c_str(),
               saved_packets_);
}

} // namespace rkcam