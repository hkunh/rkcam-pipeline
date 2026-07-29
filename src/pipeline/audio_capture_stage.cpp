#include "rkcam/pipeline/audio_capture_stage.hpp"

#include "rkcam/core/log.hpp"

#include <utility>

namespace rkcam{

AudioCaptureStage::AudioCaptureStage(
    const AudioCaptureStageConfig& config,
    std::unique_ptr<IAudioSource> source,
    BlockingQueue<PipelineAudioFrame>& output_queue)
    : config_(config),
      source_(std::move(source)),
      output_queue_(output_queue)
{
}

AudioCaptureStage::~AudioCaptureStage()
{
    stop();
}

bool AudioCaptureStage::start()
{
    if (running_) {
        return true;
    }

    if (!source_) {
        RKCAM_LOGE("[%s] start failed: audio source is null",
                   config_.stage_name.c_str());
        return false;
    }

    if(!source_->open())
    {
        RKCAM_LOGE("[%s] audio source open failed",
                   config_.stage_name.c_str());
        return false;
    }
    if(!source_->start())
    {
        RKCAM_LOGE("[%s] audio source start failed",
                   config_.stage_name.c_str());
        source_->close();
        return false;
    }

    captured_frames_ = 0;
    failed_reads_ = 0;
    failed_pushes_ = 0;
    running_ = true;
    thread_ = std::thread(&AudioCaptureStage::threadLoop, this);

    RKCAM_LOGI("[%s] AudioCaptureStage started, stream=%s",
               config_.stage_name.c_str(),
               config_.stream_id.c_str());

    return true;
}
void AudioCaptureStage::stop()
{
    if(!running_ && !thread_.joinable())
    {
        return;
    }
    running_ = false;

    /*
     * 先 stop source，尽量唤醒阻塞在 snd_pcm_readi() 的采集线程。
     */
    if(source_)
    {
        source_->stop();
    }
    if(thread_.joinable())
    {
        thread_.join();
    }

    /*
     * AudioCaptureStage 是 output_queue_ 的生产者。
     * 停止时通知下游 EOF。
     */
    output_queue_.stop();
    if(source_)
    {
        source_->close();
    }
    RKCAM_LOGI("[%s] AudioCaptureStage stopped, captured_frames=%d failed_reads=%d failed_pushes=%d",
               config_.stage_name.c_str(),
               captured_frames_,
               failed_reads_,
               failed_pushes_);
}
void AudioCaptureStage::threadLoop()
{
    while(running_)
    {
        AudioSourceFrame src_frame;
        if(!source_->readFrame(src_frame))
        {
            if(!running_)
            {
                break;
            }
            ++failed_reads_;
            if (failed_reads_ % 20 == 0) {
                RKCAM_LOGE("[%s] readFrame failed count=%d",
                           config_.stage_name.c_str(),
                           failed_reads_);
            }
            continue;
        }

        /*
         * 你的 AudioSourceFrame / PipelineAudioFrame 当前是同一个 AudioFrame alias。
         * 这里直接 move，不复制 PCM 数据。
         */
        PipelineAudioFrame out_frame = std::move(src_frame);
        if(out_frame.stream_id.empty())
        {
            out_frame.stream_id = config_.stream_id;
        }

        const int nb_samples = out_frame.nb_samples;
        const int64_t pts_us = out_frame.pts_us;
        const int64_t duration_us = out_frame.duration_us;
        const size_t size = out_frame.size();
        if(!output_queue_.push(std::move(out_frame)))
        {
            ++failed_pushes_;
            RKCAM_LOGE("[%s] output_queue push failed, failed_pushes=%d",
                       config_.stage_name.c_str(),
                       failed_pushes_);

            if (output_queue_.stopped()) {
                break;
            }
            continue;
        }

        ++captured_frames_;

        if (config_.log_interval > 0 &&
            captured_frames_ % config_.log_interval == 0) {
            RKCAM_LOGI("[%s] captured_audio_frames=%d stream=%s samples=%d pts=%lld duration=%lld size=%zu",
                    config_.stage_name.c_str(),
                    captured_frames_,
                    config_.stream_id.c_str(),
                    nb_samples,
                    static_cast<long long>(pts_us),
                    static_cast<long long>(duration_us),
                    size);
        }

        if (config_.max_frames > 0 &&
            captured_frames_ >= config_.max_frames) {
            RKCAM_LOGI("[%s] reached max_frames=%d",
                       config_.stage_name.c_str(),
                       config_.max_frames);
            break;
        }

    }

    output_queue_.stop();
    running_ = false;
    RKCAM_LOGI("[%s] AudioCaptureStage thread exit, captured_frames=%d failed_reads=%d failed_pushes=%d",
               config_.stage_name.c_str(),
               captured_frames_,
               failed_reads_,
               failed_pushes_);
}

}