#pragma once
#include "rkcam/audio/audio_frame.hpp"
#include "rkcam/core/blocking_queue.hpp"
#include "rkcam/pipeline/pipeline_stage.hpp"

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <string>
#include <thread>

namespace rkcam{

struct WavSaveStageConfig{

    std::string stage_name = "wav_save";
    /*
     * 输出 WAV 文件路径。
     * 例如：
     *   /userdata/rkcam/output/test.wav
     */

    std::string output_path;

    /*
     * <= 0 表示不限制。
     */

    int max_frames = 0;

    /*
     * 每多少帧打印一次日志。
     * <= 0 表示不打印周期日志。
     */
    int log_interval = 50;

    /*
     * 当前第一版只建议测试 S16_LE。
     */
    bool strict_format = true;

};

class WavSaveStage : public IStage{

public:
    WavSaveStage(
        const WavSaveStageConfig& config,
        BlockingQueue<PipelineAudioFrame>& input_queue
    );
    ~WavSaveStage() override;

    WavSaveStage(const WavSaveStage&) = delete;
    WavSaveStage& operator=(const WavSaveStage&) = delete;

    bool start() override;
    void stop() override;

private:
    void threadLoop();
    bool openFileFromFrame(const PipelineAudioFrame& frame);
    bool validateFrame(const PipelineAudioFrame& frame) const;

    bool writeFrame(const PipelineAudioFrame& frame);

    bool writeWavHeader(uint32_t data_size);
    bool updateWavHeader();
    void closeFile();

    static int wavBitsPerSample(AudioSampleFormat format);
    static bool writeLe16(std::FILE* fp, uint16_t value);
    static bool writeLe32(std::FILE* fp, uint32_t value);
    static bool writeBytes(std::FILE* fp, const void* data, size_t size);

private:
    WavSaveStageConfig config_;
    BlockingQueue<PipelineAudioFrame>& input_queue_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::FILE* fp_ = nullptr;

    int sample_rate_ = 0;
    int channels_ = 0;
    AudioSampleFormat format_ = AudioSampleFormat::Unknown;
    int bits_per_sample_ = 0;
    int block_align_ = 0; //指的是“音频时间帧（Frame）的逻辑边界对齐”，本质上就是计算单次完整采样的数据量大小。方便播放器的
    int byte_rate_ = 0; //播放器播放这个 WAV 文件时，每秒钟需要消耗多少字节的数据（相当于音频的字节码率）

    uint64_t data_bytes_ = 0;

    int saved_frames_ = 0;
    int failed_frames_ = 0;


};




}//namespace rkcam