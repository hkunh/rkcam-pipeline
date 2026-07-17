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
struct AVPacket;

namespace rkcam {

enum class RtspPacketWriteMode {
    /*
     * 最稳：每个 packet memcpy 到 AVPacket 内部 buffer。
     */
    Copy,

    /*
     * 推荐：AVPacket 不复制数据，而是通过 AVBufferRef 持有
     * EncodedPacket::buffer 的 shared_ptr 生命周期。
     */
    RefCountedNoCopy,
};

struct RtspVideoStreamConfig {
    bool enabled = true;

    CodecType codec = CodecType::H264;

    int width = 0;
    int height = 0;
    int fps = 30;

    /*
     * 推流开始时等待关键帧。
     * 对 H264 很重要，否则客户端可能黑屏/花屏。
     */
    bool wait_key_frame = true;
};

struct RtspAudioStreamConfig {
    /*
     * 第一版先 false。
     * 后面接 AAC 编码时改 true。
     *
     * 注意：
     *   RTSP muxer 写 header 前就必须知道是否有音频 stream。
     */
    bool enabled = false;

    CodecType codec = CodecType::AAC;

    int sample_rate = 48000;
    int channels = 2;
    int bit_rate = 128000;

    /*
     * AAC AudioSpecificConfig。
     * 后面接 AAC encoder 时，把 encoder 输出的 extradata 填到这里。
     */
    std::vector<uint8_t> extradata;
};

struct RtspPushStageConfig {
    std::string stage_name = "rtsp_push";

    /*
     * 例如：
     *   rtsp://192.168.1.100:8554/live
     *
     * 第一版建议配合 MediaMTX 测试。
     */
    std::string url;

    RtspVideoStreamConfig video;
    RtspAudioStreamConfig audio;

    bool rtsp_over_tcp = true;

    /*
     * 默认不复制码流数据。
     */
    RtspPacketWriteMode packet_write_mode =
        RtspPacketWriteMode::RefCountedNoCopy;

    /*
     * 网络输出可能阻塞/失败。
     * 第一版：失败次数超过阈值就退出。
     */
    int max_write_failures = 10;

    int log_interval = 30;
};



class RtspPushStage : public IStage{

public:
    RtspPushStage(const RtspPushStageConfig& config, BlockingQueue<EncodedPacket>& input_queue);
    ~RtspPushStage() override;

    RtspPushStage(const RtspPushStage&) = delete;
    RtspPushStage& operator=(const RtspPushStage&) = delete;

    bool start() override;
    void stop() override;

private:
    void threadLoop();

    bool initMuxerFromVideoPacket(const EncodedPacket& packet);
    bool createVideoStreamFromH264(const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps);

    bool createAudioStream();

    bool writePacket(const EncodedPacket& packet);
    bool writeVideoPacket(const EncodedPacket& packet);
    bool writeAudioPacket(const EncodedPacket& packet);

    bool writeAvPacket(const EncodedPacket& packet, AVStream* stream, bool is_key);
    bool makeAvPacketCopy(const EncodedPacket& packet, struct AVPacket& avpkt);
    bool makeAvPacketRefCountedNoCopy(const EncodedPacket& packet, struct AVPacket& avpkt);

    void closeMuxer();

    int64_t relativePtsUs(int64_t pts_us) const;

private:
    RtspPushStageConfig config_;

    BlockingQueue<EncodedPacket>& input_queue_;

    std::thread thread_;
    std::atomic<bool> running_{false};

    AVFormatContext* fmt_ctx_ = nullptr;
    AVStream* video_stream_ = nullptr;
    AVStream* audio_stream_ = nullptr;

    bool header_written_ = false;
    int64_t first_pts_us_ = -1;

    int64_t last_video_dts_ = INT64_MIN;
    int64_t last_audio_dts_ = INT64_MIN;

    int input_packets_ = 0;
    int pushed_packets_ = 0;
    int dropped_packets_ = 0;
    int write_failures_ = 0;



};






}