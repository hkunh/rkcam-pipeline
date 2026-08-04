#include "rkcam/pipeline/camera_pipeline.hpp"
#include "rkcam/core/log.hpp"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace {

volatile std::sig_atomic_t g_stop = 0;

void signalHandler(int)
{
    g_stop = 1;
}

bool ensureDir(const char* path)
{
    if (!path || path[0] == '\0') {
        return false;
    }

    struct stat st {};
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    if (mkdir(path, 0755) == 0) {
        return true;
    }

    return errno == EEXIST;
}

int parseIntArg(const char* s, int default_value)
{
    if (!s) {
        return default_value;
    }

    char* end = nullptr;
    long v = std::strtol(s, &end, 10);

    if (end == s || v <= 0) {
        return default_value;
    }

    return static_cast<int>(v);
}

} // namespace

int main(int argc, char** argv)
{
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    /*
     * 用法：
     *   ./bin/audio_wav_test
     *   ./bin/audio_wav_test /userdata/rkcam/output/test_audio.wav 10
     *
     * 参数：
     *   argv[1] = 输出 wav 路径
     *   argv[2] = 录音秒数
     */
    const std::string output_path =
        argc >= 2 ? argv[1] : "/userdata/rkcam/output/test_audio.wav";

    const int duration_sec =
        argc >= 3 ? parseIntArg(argv[2], 5) : 5;

    ensureDir("/userdata");
    ensureDir("/userdata/rkcam");
    ensureDir("/userdata/rkcam/output");

    RKCAM_LOGI("audio_wav_test start: output=%s duration=%d sec",
               output_path.c_str(),
               duration_sec);

    rkcam::CameraPipelineConfig cfg;
    cfg.stream_id = "audio_test";

    /*
     * queue: audio_capture -> wav_save
     */
    rkcam::PipelineQueueConfig audio_to_wav;
    audio_to_wav.name = "audio_to_wav";
    audio_to_wav.value_type = rkcam::PipelineQueueValueType::PipelineAudioFrame;
    audio_to_wav.capacity = 16;
    audio_to_wav.policy = rkcam::QueueFullPolicy::DropOldest;

    /*
     * AudioCaptureStage
     */
    rkcam::StageNodeConfig audio_capture;
    audio_capture.name = "audio_capture";
    audio_capture.type = rkcam::StageType::AudioCapture;
    audio_capture.output_queues = {
        audio_to_wav,
    };

    audio_capture.audio_capture.stage_name = "audio_capture";
    audio_capture.audio_capture.stream_id = "audio0";
    audio_capture.audio_capture.max_frames = 0;
    audio_capture.audio_capture.log_interval = 50;

    /*
     * 你的 arecord -l / dump-hw-params 显示：
     *   card 0 device 0: rk809-codec
     *   FORMAT: S16_LE S24_LE S32_LE
     *   CHANNELS: [2 8]
     *   RATE: [8000 96000]
     *
     * 所以第一版用：
     *   hw:0,0 / S16_LE / 48000 / 2ch
     */
    audio_capture.audio_capture.source.device = "hw:0,0";
    audio_capture.audio_capture.source.stream_id = "audio0";
    audio_capture.audio_capture.source.sample_rate = 48000;
    audio_capture.audio_capture.source.channels = 2;
    audio_capture.audio_capture.source.format = rkcam::AudioSampleFormat::S16LE;
    audio_capture.audio_capture.source.period_size = 1024;
    audio_capture.audio_capture.source.periods = 4;
    audio_capture.audio_capture.source.allow_partial_read = false;

    /*
     * WavSaveStage
     */
    rkcam::StageNodeConfig wav_save;
    wav_save.name = "wav_save";
    wav_save.type = rkcam::StageType::WavSave;
    wav_save.input_queues[0] = audio_to_wav;

    wav_save.wav_save.stage_name = "wav_save";
    wav_save.wav_save.output_path = output_path;
    wav_save.wav_save.max_frames = 0;
    wav_save.wav_save.log_interval = 50;
    wav_save.wav_save.strict_format = true;

    cfg.nodes = {
        audio_capture,
        wav_save,
    };

    rkcam::CameraPipeline pipeline(cfg);

    if (!pipeline.start()) {
        RKCAM_LOGE("audio_wav_test: CameraPipeline start failed");
        return 1;
    }

    for (int i = 0; i < duration_sec && !g_stop; ++i) {
        sleep(1);
    }

    RKCAM_LOGI("audio_wav_test stopping...");
    pipeline.stop();

    RKCAM_LOGI("audio_wav_test done: output=%s",
               output_path.c_str());

    return 0;
}