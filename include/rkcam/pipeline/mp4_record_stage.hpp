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
#include <future>
#include <chrono>
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

    std::string output_path;  //为了支持动态录像，这个路径废弃了，不再由初始配置信息固定传入

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
// struct EncodedPacketInputPort{
//     std::string port_name;
//     /*
//      * 用于检查上游有没有接错。
//      */
//     std::string stream_id;
//     MediaType media_type = MediaType::Unknown;
//     CodecType codec = CodecType::Unknown;
//     /*
//      * 非拥有指针。
//      * 队列生命周期必须长于 Mp4RecordStage。
//      */
//     BlockingQueue<EncodedPacket>* queue = nullptr;

//     /*
//      * 当前先保留。
//      * 后面可用于“音频异常时是否允许仅视频继续录像”。
//      */
//     bool required = true;
// };
enum class RecordingState{
    /*
    *空闲状态，初始化后没有接收到录制命令，会忽略不是command类型的packet
    */
    Idle,
    /*
     * 已收到开始命令，正在等待第一帧 IDR。
     */
    Starting,
    /*
     * MP4 header 已写，正在录像。
     */
    Recording,
    /*
     * 正在写 trailer 并关闭当前文件。
     */
    Stopping,
    Error,
};
class Mp4RecordStage : public IStage{
public:
    Mp4RecordStage(const Mp4RecordStageConfig& config, const std::vector<EncodedPacketInputPort>& input_ports);
    ~Mp4RecordStage() override;

    Mp4RecordStage(const Mp4RecordStage&) = delete;
    Mp4RecordStage& operator=(const Mp4RecordStage&) = delete;

    bool start() override;
    void stop() override;

    /*
     * beginRecording 返回 true：
     *   表示 Mux Writer 已接受命令并进入 Starting。
     *
     * 此时还没有真正创建 MP4，
     * 需要等待下一帧 IDR。
     */
    bool beginRecording(const std::string& output_path, const std::vector<uint8_t>& video_extradata, const uint64_t request_time_us);
    /*
     * 同步等待 Mux Writer 写 trailer 并关闭文件。
     */
    bool endRecording();
    RecordingState recordingState() const;
    bool isRecording() const
    {
        return recordingState() == RecordingState::Recording;
    }

    std::string currentOutputPath() const;

private:
    enum class InputEventType{
        Packet,
        Command,
        Eos
    };
    enum class RecordCommandType{
        Begin,
        End,
    };
    struct RecordCommand{
        RecordCommandType type = RecordCommandType::Begin;

        std::string output_path;
        /*
        * 使用和音视频 PTS 相同的 CLOCK_MONOTONIC 时间域。
        * 防止命令前已经积压的 packet 被写进新文件。
        */
        int64_t request_time_us = -1;
        /*
        * 控制线程等待 Mux Writer 确认命令。
        */
        /*
        *这里的std::shared_ptr是为了在共享std::promise<bool>变量，因为std::promise是不能拷贝的
        *std::promise是一种消息传递类，是为了能够在不同线程传递消息，可通过future.get() 挂起等待结果，而不是轮询等其他麻烦的操作
        */
        std::shared_ptr<std::promise<bool>> completion; 

        std::vector<uint8_t> video_extradata;
    };
    struct InputEvent{
        InputEventType type = InputEventType::Packet;
        size_t port_index = 0;
        EncodedPacket packet;
        RecordCommand command;
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

    //-------------------------------------------
    bool postCommandAndWait(RecordCommand&& command);
    bool handleRecordCommand(RecordCommand&& command);
    bool beginRecordingInMuxThread(
        const std::string& output_path,
        int64_t request_time_us,
        const std::vector<uint8_t>& video_extradata
    );
    bool endRecordingInMuxThread();
    
    void resetRecordingSession();
    
    /*
    *检查是否有SPS PPS，没有就从packet中提取SPS PPS并缓存
    */
    bool tryUpdateSessionVideoExtradata(const EncodedPacket& packet);

    int64_t packetTimestampUs(const EncodedPacket& packet) const;


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
    
    /*---------------------------------------*/
    std::atomic<RecordingState> recording_state_{
        RecordingState::Idle
    };

    /*
     * 只由 Mux Writer 读写。
     */
    std::string active_output_path_;
    int64_t start_request_pts_us_ = -1;

    /*
    * 当前录像会话的 H.264 extradata。
    *
    * 生命周期：
    *   beginRecording() -> 清空
    *   Starting -> 从当前编码器输出重新获取
    *   MP4 header 创建后可以保留到会话结束
    *   endRecording() -> 清空
    *
    * 不允许跨录像会话复用。
    */
    std::vector<uint8_t> session_video_extradata_;

    mutable std::mutex state_mutex_;
    uint64_t idle_dropped_packets_ = 0;
};

}// namespace rkcam