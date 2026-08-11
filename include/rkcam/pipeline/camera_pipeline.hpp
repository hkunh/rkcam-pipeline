#pragma once

#include "rkcam/pipeline/pipeline_queue.hpp"

#include "rkcam/pipeline/capture_stage.hpp"
#include "rkcam/pipeline/fps_stage.hpp"
#include "rkcam/pipeline/raw_save_stage.hpp"
#include "rkcam/pipeline/rga_stage.hpp"
#include "rkcam/pipeline/pipeline_stage.hpp"
#include "rkcam/pipeline/mpp_stage.hpp"
#include "rkcam/pipeline/encoded_save_stage.hpp"
#include "rkcam/pipeline/mp4_record_stage.hpp"
#include "rkcam/pipeline/display_stage.hpp"
#include "rkcam/platform/linux/drm/drm_display_sink.hpp"
#include "rkcam/pipeline/videoframe_tee_stage.hpp"
#include "rkcam/pipeline/encoded_packet_tee_stage.hpp"
#include "rkcam/pipeline/rtsp_push_stage.hpp"

#include "rkcam/pipeline/audio_capture_stage.hpp"
#include "rkcam/pipeline/wav_save_stage.hpp"
#include "rkcam/pipeline/aac_encode_stage.hpp"
#include "rkcam/pipeline/aac_adts_save_stage.hpp"

#include "rkcam/video/video_frame.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace rkcam {

enum class StageType {
    Capture,
    AudioCapture,
    WavSave,
    AacEncode,
    AacAdtsSave,

    Rga,
    Mpp,
    EncodedSave,
    Mp4Record,
    RtspPush,
    Fps,
    RawSave,
    Display,
    VideoFrameTee,
    EncodedPacketTee,
};


struct StageNodeConfig{
    std::string name;
    StageType type = StageType::Capture;

    /*
     * 所有输入统一使用数组。
     *
     * source stage:
     *   input_queues = {};
     *
     * 普通单输入 stage:
     *   input_queues = { input };
     *
     * 多输入 stage:
     *   input_queues = { video_input, audio_input };
     */
    std::vector<PipelineQueueConfig> input_queues;
    /*
     * 所有输出统一使用数组。
     *
     * 单输出 stage:
     *   output_queues = { q };
     *
     * sink stage:
     *   output_queues = {};
     *
     * tee stage:
     *   output_queues = { q1, q2, ... };
     */
    std::vector<PipelineQueueConfig> output_queues;

    /*
    * 每个 stage 自己的配置。
    * 第一版不用 std::variant，简单直接。
    */
    CaptureStageConfig capture;
    RgaStageConfig rga;
    MppStageConfig mpp;
    EncodedSaveStageConfig encoded_save;
    Mp4RecordStageConfig mp4_record;
    RtspPushStageConfig rtsp_push;
    FpsStageConfig fps;
    RawSaveStageConfig raw_save;

    DisplayStageConfig display;
    DrmDisplaySinkConfig drm_display;

    VideoFrameTeeStageConfig video_frame_tee;
    EncodedPacketTeeStageConfig encoded_packet_tee;

    AudioCaptureStageConfig audio_capture;
    WavSaveStageConfig wav_save;
    AacEncodeStageConfig aac_encode;
    AacAdtsSaveStageConfig aac_adts_save;

};

struct StageNode{
    StageNodeConfig config;

    std::vector<std::shared_ptr<IPipelineQueue>> input_queues;
    std::unique_ptr<IStage> stage;
    std::vector<std::shared_ptr<IPipelineQueue>> output_queues;
};

struct CameraPipelineConfig {
    std::string stream_id = "cam0";
    /*
     * 按 上游 -> 下游 顺序填写。
     *
     * 例如：
     *   capture -> rga -> raw_save
     */
    std::vector<StageNodeConfig> nodes;
};
class CameraPipeline{
public: 
    explicit CameraPipeline(const CameraPipelineConfig& config);
    ~CameraPipeline();

    CameraPipeline(const CameraPipeline&) = delete;
    CameraPipeline& operator=(const CameraPipeline&) = delete;

    bool start();
    void stop();

    bool isRunning() const;

    /*-------------------------------*/
    bool startRecording(const std::string& path);
    bool stopRecording();
    RecordingState recordingState() const;

    bool startStreaming(const std::string& url);
    bool stopStreaming();
    StreamingState streamingState() const;
private:
    bool initStageNodes();
    bool initStageNodeQueues();
    bool initStageNodeStages();

    bool createOrReuseQueue(const PipelineQueueConfig& config, std::shared_ptr<IPipelineQueue>& queue);

    bool createStageForNode(StageNode& node);

    bool startStages();
    void stopStages();

    void stopAllQueues();
    void clearAllQueues();

    void destroy();




private:
    CameraPipelineConfig config_;

    /*
     * 所有队列按 name 统一管理。
     *
     * 注意：
     *   现在 value 是 IPipelineQueue。
     *   里面实际可能是：
     *     VideoFrameQueueBox
     *     EncodedPacketQueueBox
     */
     std::unordered_map<std::string, std::shared_ptr<IPipelineQueue>> queue_map_;
     
    /*
    * 每个节点包含：
    *   input queue config + input queue
    *   stage
    *   output queue config + output queue
    */
    std::vector<StageNode> nodes_;

    std::atomic<bool> running_{false};

    /*---------------------------------------*/
    MppStage* mpp_stage_ = nullptr;
    Mp4RecordStage* mp4_record_stage_ = nullptr;
    RtspPushStage* rtsp_push_stage_ = nullptr;
    /*
     * 串行化录像 / 推流控制操作。
     *
     * MPP encoder是录像和推流共享资源，
     * 所以 startRecording / startStreaming
     * 都必须经过这个锁协调。
     */
    std::mutex control_mutex_;
};

} // namespace rkcam