#pragma once
#include "rkcam/audio/audio_frame.hpp"
#include "rkcam/audio/audio_source.hpp"
#include "rkcam/audio/alsa_audio_source.hpp"
#include "rkcam/core/blocking_queue.hpp"
#include "rkcam/pipeline/pipeline_stage.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace rkcam{

struct AudioCaptureStageConfig{
    std::string stage_name = "audio_capture";
    std::string stream_id = "audio0";

    /*
     * 第一版固定使用 ALSA。
     * 后面如果你要支持其他音频源，可以再加 source_type。
     */
    AlsaAudioSourceConfig source;
    int max_frames = 0;

    int log_interval = 50;

};

class AudioCaptureStage : public IStage{
public:
    AudioCaptureStage(
        const AudioCaptureStageConfig& config,
        std::unique_ptr<IAudioSource> source,
        BlockingQueue<PipelineAudioFrame>& output_queue);

    ~AudioCaptureStage() override;

    AudioCaptureStage(const AudioCaptureStage&) = delete;
    AudioCaptureStage& operator=(const AudioCaptureStage&) = delete;

    bool start() override;
    void stop() override;

private:
    void threadLoop();

private:
    AudioCaptureStageConfig config_;

    std::unique_ptr<IAudioSource> source_;

    BlockingQueue<PipelineAudioFrame>& output_queue_;

    std::thread thread_;

    std::atomic<bool> running_{false};

    int captured_frames_ = 0;
    int failed_reads_ = 0;
    int failed_pushes_ = 0;

};


}