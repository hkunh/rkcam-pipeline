#pragma once

#include "rkcam/core/blocking_queue.hpp"
#include "rkcam/media/encoded_packet.hpp"
#include "rkcam/pipeline/pipeline_stage.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>
#include <condition_variable>
#include <deque>
#include <mutex>

struct AVFormatContext;
struct AVStream;

namespace rkcam{


struct Mp4VideoStreamConfig{

    bool enabled = true;
    std::string  stream_id = "cam0";

    CodecType codec = CodecType::H264;

    int width = 0;
    int height = 0;
    int fps = 30;
    int bit_rate = 0;
    /*
     * 短期可以为空：
     *   为空 -> 继续从第一帧 H264 packet 提取 SPS/PPS。
     *
     * 后面优化：
     *   MppEncoder 拿到 SPS/PPS 后填这里，MP4 就可以启动时直接初始化。
     */
    std::vector<uint8_t> extradata;
}; 



struct Mp4AudioStreamConfig{

    bool enabled = false;

    std::string stream_id = "audio0";
    CodecType codec = CodecType::AAC;

    int sample_rate = 48000;
    int channels = 1;
    int bit_rate = 64000;

    /*
     * AAC AudioSpecificConfig。
     * 如果为空，Mp4RecordStage 会按 AAC-LC 自动生成。
     */
    std::vector<uint8_t> extradata;

};


struct Mp4RecordStageConfig{
    std::string stage_name = "mp4_record";

    std::string output_path;

    Mp4VideoStreamConfig video;
    Mp4AudioStreamConfig audio;

    int log_interval = 30;

    /*
     * reader -> mux writer 的内部队列容量。
     * 这是 event 数量，不是字节数。
     */
    size_t internal_queue_capacity = 256;

    /*
     * 等待 H264 SPS/PPS 时，最多缓存多少 AAC packet。
     */
    size_t max_pending_audio_packets = 128;
    
};
struct EncodedPacketInputPort{
    std::string port_name;
    /*
     * 用于检查上游有没有接错。
     */
    std::string stream_id;
    MediaType media_type = MediaType::Unknown;
    CodecType codec = CodecType::Unknown;
    /*
     * 非拥有指针。
     * 队列生命周期必须长于 Mp4RecordStage。
     */
    BlockingQueue<EncodedPacket>* queue = nullptr;

    /*
     * 当前先保留。
     * 后面可用于“音频异常时是否允许仅视频继续录像”。
     */
    bool required = true;
};

class Mp4RecordStage : public IStage{
public:
    Mp4RecordStage(const Mp4RecordStageConfig& config, const std::vector<EncodedPacketInputPort>& input_ports);
    ~Mp4RecordStage() override;

    Mp4RecordStage(const Mp4RecordStage&) = delete;
    Mp4RecordStage& operator=(const Mp4RecordStage&) = delete;

    bool start() override;
    void stop() override;

private:
    enum class InputEventType{
        Packet,
        Eos
    };
    struct InputEvent{
        InputEventType type = InputEventType::Packet;
        size_t port_index = 0;
        EncodedPacket packet;
    };

private:


    bool normalizeAndValidateConfig();
    bool validateInputPorts() const;

    /*
     * 每个外部输入队列一个 reader。
     * reader 不允许操作 FFmpeg。
     */
    void inputReaderLoop(size_t port_index);
    /*
     * 唯一允许操作 fmt_ctx_ 的线程。
     */
    void muxWriterLoop();

    // bool pushInternalEvent(InputEvent&& event);
    // bool popInternalEvent(InputEvent& event);
    // void closeInternalQueue();
    // void resetInternalQueue();

    bool handleInputPacket(size_t port_index, EncodedPacket&& packet);  //处理输入数据包 注意，这个port_index只是打印以及保险验证用的，可以设计成不用传入这个
    bool validatePacketForPort(const EncodedPacketInputPort& port, const EncodedPacket& packet) const;
    bool handlePacketBeforeHeader(EncodedPacket&& packet);
    bool initializeFromFirstVideoPacket(EncodedPacket&& video_packet);
    bool flushAudioPackets(EncodedPacket&& first_video_packet);
    bool initMuxerWithVideoExtradata(const std::vector<uint8_t>& video_extradata, int64_t start_pts_us);


    
    bool addVideoStream(const std::vector<uint8_t>& extradata);
    bool addAudioStream();

    bool writePacket(const EncodedPacket& packet);
    bool writeVideoPacket(const EncodedPacket& packet);
    bool writeAudioPacket(const EncodedPacket& packet);

    bool writePacketToStream(const EncodedPacket& packet, AVStream* stream, int64_t default_duration_us, bool is_video);

    void accountSavedPacket(const EncodedPacket& packet);

    void closeMuxer();

private:
    Mp4RecordStageConfig config_;

    /*
     * 复制端口描述
     */
    std::vector<EncodedPacketInputPort> input_ports_;
    /*
     * 每个 input port 一个 reader thread。
     */
    std::vector<std::thread> input_threads_;

    /*
     * 唯一 mux writer。
     */
    std::thread mux_thread_;
    std::atomic<bool> running_{false};


    /*
     * 多个 reader -> 一个 mux writer。
     * 复用通用 BlockingQueue。
     */
    BlockingQueue<InputEvent> internal_queue_;
    /*
     * 只由 mux thread 修改。
     */
    std::vector<bool> input_eos_;

    /*
     * 视频 SPS/PPS 到来前缓存的音频。
     */
    std::deque<EncodedPacket> pending_audio_packets_;

    AVFormatContext* fmt_ctx_ = nullptr;
    AVStream* video_stream_ = nullptr;
    AVStream* audio_stream_ = nullptr;

    bool header_written_ = false;
    int64_t first_pts_us_ = -1;

    int saved_packets_ = 0;
    int saved_video_packets_ = 0;
    int saved_audio_packets_ = 0;
    int failed_packets_ = 0;
    int dropped_audio_before_header_ = 0;
    
};

}// namespace rkcam