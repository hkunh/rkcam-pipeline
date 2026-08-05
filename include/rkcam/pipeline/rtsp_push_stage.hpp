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
    /*
     * 为空时，由 CameraPipeline 使用自己的 config_.stream_id。
     */
    std::string stream_id;
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
    /*
     * 应当与 AacEncodeStage 输出的 stream_id 一致。
     */
    std::string stream_id = "audio0";
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
     * H264压缩码流保持零拷贝。
     */
    RtspPacketWriteMode video_write_mode =
        RtspPacketWriteMode::RefCountedNoCopy;
    /*
     * AAC packet很小，第一版复制更加简单可靠。
     */
    RtspPacketWriteMode audio_write_mode =
        RtspPacketWriteMode::Copy;

    /*
     * 等待第一帧视频IDR期间最多缓存多少AAC packet。
     *
     * 48kHz / 1024 samples:
     * 64包约等于1.36秒。
     */
    size_t max_pending_audio_packets = 64;
    /*
     * FFmpeg音视频交织最长允许缓存时间。
     */
    int64_t max_interleave_delta_us =5000; //50ms

    /*
     * 避免MediaMTX再次拆分过大的RTP packet。
     */
    int rtp_packet_size = 1400;


    /*
     * 网络输出可能阻塞/失败。
     * 第一版：失败次数超过阈值就退出。
     */
    int max_write_failures = 10;

    int log_interval = 30;
};



class RtspPushStage : public IStage{

public:
    RtspPushStage(
        const RtspPushStageConfig& config,
        const std::vector<EncodedPacketInputPort>& input_ports);
    ~RtspPushStage() override;

    RtspPushStage(const RtspPushStage&) = delete;
    RtspPushStage& operator=(const RtspPushStage&) = delete;

    bool start() override;
    void stop() override;

private:
    enum class InputEventType{
        Packet,
        Eos,
    };
    struct InputEvent{
        InputEventType type = InputEventType::Packet;
        size_t port_index = 0;
        EncodedPacket packet;
    };

private:

    bool normalizeAndValidateConfig();
    bool validateInputPorts() const;

    void inputReaderLoop(size_t port_index);
    void muxWriterLoop();
    void stopAllInputQueues();

    bool validatePacketForPort(const EncodedPacketInputPort& port, const EncodedPacket& packet)const;

    bool handleInputPacket(size_t port_index, EncodedPacket&& packet);
    bool handlePacketBeforeHeader(EncodedPacket&& packet);

    bool initMuxerFromVideoPacket(const EncodedPacket& packet);

    bool flushPendingAudio(EncodedPacket&& first_video_packet);


    bool createVideoStreamFromH264(const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps);

    bool createAudioStream();

    bool writePacket(const EncodedPacket& packet);
    bool writeVideoPacket(const EncodedPacket& packet);
    bool writeAudioPacket(const EncodedPacket& packet);

    bool writeAvPacket(const EncodedPacket& packet, AVStream* stream, bool is_key);
    bool makeAvPacketCopy(const EncodedPacket& packet, struct AVPacket& avpkt);
    bool makeAvPacketRefCountedNoCopy(const EncodedPacket& packet, struct AVPacket& avpkt);

    void accountPushedPacket(const EncodedPacket& packet);

    void closeMuxer();

    int64_t relativePtsUs(int64_t pts_us) const;

private:
    RtspPushStageConfig config_;

    std::vector<EncodedPacketInputPort> input_ports_;
    std::vector<std::thread> input_threads_;
    
    /*
     * 唯一允许操作AVFormatContext的线程。
     */

    std::thread mux_thread_;
    std::atomic<bool> running_{false};

    BlockingQueue<InputEvent> internal_queue_;
    std::vector<bool> input_eos_;

    /*
     * RTSP header创建前暂存AAC。
     */
    std::deque<EncodedPacket> pending_audio_packets_;

    AVFormatContext* fmt_ctx_ = nullptr;
    AVStream* video_stream_ = nullptr;
    AVStream* audio_stream_ = nullptr;

    bool header_written_ = false;
    int64_t first_pts_us_ = -1;

    int64_t last_video_dts_ = INT64_MIN;
    int64_t last_audio_dts_ = INT64_MIN;

    uint64_t input_packets_ = 0;
    uint64_t pushed_packets_ = 0;
    uint64_t pushed_video_packets_ = 0;
    uint64_t pushed_audio_packets_ = 0;
    uint64_t dropped_packets_ = 0;
    uint64_t write_failures_ = 0;

};






}