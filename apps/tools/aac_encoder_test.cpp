#include "rkcam/pipeline/camera_pipeline.hpp"
#include "rkcam/core/log.hpp"

#include <cerrno>
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
    const long v = std::strtol(s, &end, 10);

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
     *
     *   ./bin/audio_aac_test
     *
     *   ./bin/audio_aac_test \
     *      /userdata/rkcam/output/pipeline_audio.aac \
     *      5 \
     *      64000
     *
     * 参数：
     *   argv[1] = /userdata/rkcam/output/pipeline_audio.aac \
     *      5 \
     *      64000 输出 AAC 路径
     *   argv[2] = 录制秒数
     *   argv[3] = AAC 码率
     */
    const std::string output_path =
        argc >= 2 ? argv[1] : "/userdata/rkcam/output/pipeline_audio.aac";

    const int duration_sec =
        argc >= 3 ? parseIntArg(argv[2], 5) : 5;

    const int bit_rate =
        argc >= 4 ? parseIntArg(argv[3], 64000) : 64000;

    ensureDir("/userdata");
    ensureDir("/userdata/rkcam");
    ensureDir("/userdata/rkcam/output");

    RKCAM_LOGI("audio_aac_test start: output=%s duration=%d sec bitrate=%d",
               output_path.c_str(),
               duration_sec,
               bit_rate);

    rkcam::CameraPipelineConfig cfg;
    cfg.stream_id = "audio_aac_test";

    /*
     * queue: audio_capture -> aac_encode
     */
    rkcam::PipelineQueueConfig audio_to_aac;
    audio_to_aac.name = "audio_to_aac";
    audio_to_aac.value_type = rkcam::PipelineQueueValueType::PipelineAudioFrame;
    audio_to_aac.capacity = 16;
    audio_to_aac.policy = rkcam::QueueFullPolicy::DropOldest;

    /*
     * queue: aac_encode -> aac_adts_save
     */
    rkcam::PipelineQueueConfig aac_to_save;
    aac_to_save.name = "aac_to_save";
    aac_to_save.value_type = rkcam::PipelineQueueValueType::EncodedPacket;
    aac_to_save.capacity = 16;
    aac_to_save.policy = rkcam::QueueFullPolicy::DropOldest;

    /*
     * AudioCaptureStage
     *
     * 你的板子 arecord -l:
     *   card 0, device 0: rk809-codec
     *
     * dump-hw-params:
     *   FORMAT: S16_LE S24_LE S32_LE
     *   CHANNELS: [2 8]
     *   RATE: [8000 96000]
     *
     * 所以第一版：
     *   hw:0,0 / S16_LE / 48000 / 2ch
     */
    rkcam::StageNodeConfig audio_capture;
    audio_capture.name = "audio_capture";
    audio_capture.type = rkcam::StageType::AudioCapture;
    audio_capture.output_queues = {
        audio_to_aac,
    };

    audio_capture.audio_capture.stage_name = "audio_capture";
    audio_capture.audio_capture.stream_id = "audio0";
    audio_capture.audio_capture.max_frames = 0;
    audio_capture.audio_capture.log_interval = 50;

    audio_capture.audio_capture.source.device = "hw:0,0";
    audio_capture.audio_capture.source.stream_id = "audio0";
    audio_capture.audio_capture.source.sample_rate = 48000;
    audio_capture.audio_capture.source.channels = 2;
    audio_capture.audio_capture.source.format = rkcam::AudioSampleFormat::S16LE;
    audio_capture.audio_capture.source.period_size = 1024;
    audio_capture.audio_capture.source.periods = 4;
    audio_capture.audio_capture.source.allow_partial_read = false;

    /*
     * AacEncodeStage
     *
     * 输入：
     *   S16_LE / 48000 / 2ch
     *
     * 你已经验证：
     *   只有左声道有效，右声道静音
     *
     * 所以编码：
     *   LeftToMono -> AAC-LC / 48000 / mono / 64kbps
     */
    rkcam::StageNodeConfig aac_encode;
    aac_encode.name = "aac_encode";
    aac_encode.type = rkcam::StageType::AacEncode;
    aac_encode.input_queues[0] = audio_to_aac;
    aac_encode.output_queues = {
        aac_to_save,
    };

    aac_encode.aac_encode.stage_name = "aac_encode";
    aac_encode.aac_encode.stream_id = "audio0";
    aac_encode.aac_encode.max_frames = 0;
    aac_encode.aac_encode.log_interval = 50;
    aac_encode.aac_encode.max_encode_failures = 20;

    aac_encode.aac_encode.encoder.stream_id = "audio0";
    aac_encode.aac_encode.encoder.input_sample_rate = 48000;
    aac_encode.aac_encode.encoder.input_channels = 2;
    aac_encode.aac_encode.encoder.input_format = rkcam::AudioSampleFormat::S16LE;

    aac_encode.aac_encode.encoder.output_sample_rate = 48000;
    aac_encode.aac_encode.encoder.output_channels = 1;
    aac_encode.aac_encode.encoder.bit_rate = bit_rate;
    aac_encode.aac_encode.encoder.channel_select =
        rkcam::AudioChannelSelect::LeftToMono;

    /*
     * AacAdtsSaveStage
     *
     * AacEncodeStage 输出 raw AAC。
     * 这里加 ADTS header，保存成可以直接播放的 .aac。
     */
    rkcam::StageNodeConfig aac_save;
    aac_save.name = "aac_adts_save";
    aac_save.type = rkcam::StageType::AacAdtsSave;
    aac_save.input_queues[0] = aac_to_save;

    aac_save.aac_adts_save.stage_name = "aac_adts_save";
    aac_save.aac_adts_save.output_path = output_path;
    aac_save.aac_adts_save.sample_rate = 48000;
    aac_save.aac_adts_save.channels = 1;
    aac_save.aac_adts_save.max_packets = 0;
    aac_save.aac_adts_save.log_interval = 50;
    aac_save.aac_adts_save.strict_packet = true;

    cfg.nodes = {
        audio_capture,
        aac_encode,
        aac_save,
    };

    rkcam::CameraPipeline pipeline(cfg);

    if (!pipeline.start()) {
        RKCAM_LOGE("audio_aac_test: CameraPipeline start failed");
        return 1;
    }

    for (int i = 0; i < duration_sec && !g_stop; ++i) {
        sleep(1);
    }

    RKCAM_LOGI("audio_aac_test stopping...");
    pipeline.stop();

    RKCAM_LOGI("audio_aac_test done: output=%s",
               output_path.c_str());

    return 0;
}