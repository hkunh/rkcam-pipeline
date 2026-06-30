#pragma once

#include "rkcam/core/blocking_queue.hpp"
#include "rkcam/media/encoded_packet.hpp"
#include "rkcam/pipeline/pipeline_stage.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

struct AVFormatContext;
struct AVStream;

namespace rkcam{

struct Mp4RecordStageConfig{
    std::string stage_name = "mp4_record";

    std::string output_path;

    int width = 0;
    int height = 0;
    int fps = 30;

    /*
    * 第一版只支持 H264。
    */
    CodecType codec = CodecType::H264;
    
};


class Mp4RecordStage : public IStage{
public:
    Mp4RecordStage(const Mp4RecordStageConfig& config, BlockingQueue<EncodedPacket>& input_queue);
    ~Mp4RecordStage() override;

    Mp4RecordStage(const Mp4RecordStage&) = delete;
    Mp4RecordStage& operator=(const Mp4RecordStage&) = delete;

    bool start() override;
    void stop() override;

private:
    void threadLoop();

    bool initMuxerFromPacket(const EncodedPacket& packet);
    bool writePacket(const EncodedPacket& packet);
    void closeMuxer();

private:
    Mp4RecordStageConfig config_;
    BlockingQueue<EncodedPacket>& input_queue_;

    std::thread thread_;
    std::atomic<bool> running_{false};

    AVFormatContext* fmt_ctx_ = nullptr;
    AVStream* video_stream_ = nullptr;

    bool header_written_ = false;
    int64_t first_pts_us_ = -1;

    int saved_packets_ = 0;
    int failed_packets_ = 0;
    
};

}