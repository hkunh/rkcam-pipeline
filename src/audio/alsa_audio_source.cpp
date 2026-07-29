#include "rkcam/audio/alsa_audio_source.hpp"

#include "rkcam/core/log.hpp"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <memory>

namespace rkcam {
namespace {

static snd_pcm_format_t toAlsaFormat(AudioSampleFormat format)
{
    switch(format){
        case AudioSampleFormat::S16LE:
            return SND_PCM_FORMAT_S16_LE;
        case AudioSampleFormat::S24LE:
            return SND_PCM_FORMAT_S24_LE;
        case AudioSampleFormat::S32LE:
            return SND_PCM_FORMAT_S32_LE;
        default:
            return SND_PCM_FORMAT_UNKNOWN;
    }
}
static const char* alsaErrorString(int err)
{
    return snd_strerror(err);
}

}//namespace


AlsaAudioSource::AlsaAudioSource(const AlsaAudioSourceConfig& config) : config_(config)
{

}

AlsaAudioSource::~AlsaAudioSource()
{
    close();
}


bool AlsaAudioSource::open()
{
    if (opened_) {
        return true;
    }

    if (config_.device.empty()) {
        RKCAM_LOGE("AlsaAudioSource open failed: device is empty");
        return false;
    }

    if (config_.sample_rate <= 0 ||
        config_.channels <= 0 ||
        config_.period_size <= 0 ||
        config_.periods <= 0) {
        RKCAM_LOGE("AlsaAudioSource invalid config: rate=%d channels=%d period_size=%d periods=%d",
                   config_.sample_rate,
                   config_.channels,
                   config_.period_size,
                   config_.periods);
        return false;
    }

    const snd_pcm_format_t alsa_format = toAlsaFormat(config_.format);
    if (alsa_format == SND_PCM_FORMAT_UNKNOWN) {
        RKCAM_LOGE("AlsaAudioSource unsupported sample format=%d",
                   static_cast<int>(config_.format));
        return false;
    }

    int ret = snd_pcm_open(
        reinterpret_cast<snd_pcm_t**>(&handle_),  //这个函数要求snd_pcm_t**,因为原文件再次定义了typedef struct _snd_pcm snd_pcm_t;，所以我们头文件定义的是_snd_pcm* handle_
        config_.device.c_str(),
        SND_PCM_STREAM_CAPTURE,
        0  //决定这个句柄是阻塞模式还是非阻塞模式. 0为阻塞模式
    );
    if (ret < 0) {
        RKCAM_LOGE("snd_pcm_open failed: device=%s err=%s",
                   config_.device.c_str(),
                   alsaErrorString(ret));
        handle_ = nullptr;
        return false;
    }

    if(!configureHwParams())
    {
        close();
        return false;
    }
    if(!configureSwParams())
    {
        close();
        return false;
    }

    ret = snd_pcm_prepare(reinterpret_cast<snd_pcm_t*>(handle_));
    if (ret < 0) {
        RKCAM_LOGE("snd_pcm_prepare failed: %s",
                   alsaErrorString(ret));
        close();
        return false;
    }

    frame_id_ = 0;
    opened_ = true;

    RKCAM_LOGI("AlsaAudioSource opened: device=%s stream=%s rate=%d channels=%d format=%s period_size=%d periods=%d bytes_per_frame=%d",
               config_.device.c_str(),
               config_.stream_id.c_str(),
               actual_sample_rate_,
               actual_channels_,
               audioSampleFormatToString(actual_format_),
               actual_period_size_,
               actual_periods_,
               bytesPerFrame());

    return true;
}

bool AlsaAudioSource::configureHwParams()
{
    auto* pcm = reinterpret_cast<snd_pcm_t*>(handle_);
    if(!pcm)
    {
        return false;
    }

    snd_pcm_hw_params_t* hw_params = nullptr;
    snd_pcm_hw_params_alloca(&hw_params);

    int ret = snd_pcm_hw_params_any(pcm, hw_params);
    if(ret < 0)
    {
        RKCAM_LOGE("snd_pcm_hw_params_any failed: %s",
                   alsaErrorString(ret));
        return false;
    }

    ret = snd_pcm_hw_params_set_access(pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (ret < 0) {
        RKCAM_LOGE("snd_pcm_hw_params_set_access RW_INTERLEAVED failed: %s",
                   alsaErrorString(ret));
        return false;
    }

    const snd_pcm_format_t alsa_format = toAlsaFormat(config_.format);
    ret = snd_pcm_hw_params_set_format(pcm, hw_params, alsa_format);
    if (ret < 0) {
        RKCAM_LOGE("snd_pcm_hw_params_set_format %s failed: %s",
                   audioSampleFormatToString(config_.format),
                   alsaErrorString(ret));
        return false;
    }

    unsigned int rate = static_cast<unsigned int>(config_.sample_rate);
    ret = snd_pcm_hw_params_set_rate_near(pcm, hw_params, &rate, nullptr);  //nullptr这个参数是如果找不到配置的参数，可以优先向上或者向下微调。传nullptr代表无所谓
    if (ret < 0) {
        RKCAM_LOGE("snd_pcm_hw_params_set_rate_near %d failed: %s",
                   config_.sample_rate,
                   alsaErrorString(ret));
        return false;
    }

    if(static_cast<int>(rate) != config_.sample_rate)
    {
        RKCAM_LOGI("ALSA adjusted sample_rate: request=%d actual=%u",
                   config_.sample_rate,
                   rate);
    }

    unsigned int channels = static_cast<unsigned int>(config_.channels);
    ret = snd_pcm_hw_params_set_channels_near(
        pcm,
        hw_params,
        &channels
    );
    if (ret < 0) {
        RKCAM_LOGE("snd_pcm_hw_params_set_channels_near %d failed: %s",
                   config_.channels,
                   alsaErrorString(ret));
        return false;
    }
    if (static_cast<int>(channels) != config_.channels) {
        RKCAM_LOGI("ALSA adjusted channels: request=%d actual=%u",
                   config_.channels,
                   channels);
    }

    snd_pcm_uframes_t period_size = static_cast<snd_pcm_uframes_t>(config_.period_size);
    ret = snd_pcm_hw_params_set_period_size_near(
        pcm,
        hw_params,
        &period_size,
        nullptr
    );

    unsigned int periods = static_cast<unsigned int>(config_.periods);
    ret = snd_pcm_hw_params_set_periods_near(
        pcm,
        hw_params,
        &periods,
        nullptr
    );
    if (ret < 0) {
        RKCAM_LOGE("snd_pcm_hw_params_set_periods_near %d failed: %s",
                   config_.periods,
                   alsaErrorString(ret));
        return false;
    }

    ret = snd_pcm_hw_params(pcm, hw_params);
    if (ret < 0) {
        RKCAM_LOGE("snd_pcm_hw_params apply failed: %s",
                   alsaErrorString(ret));
        return false;
    }

    snd_pcm_uframes_t actual_period_size = 0;
    unsigned int actual_periods = 0;
    snd_pcm_hw_params_get_period_size(
        hw_params,
        &actual_period_size,
        nullptr
    );
    snd_pcm_hw_params_get_periods(
        hw_params,
        &actual_periods,
        nullptr
    );

    actual_sample_rate_ = static_cast<int>(rate);
    actual_channels_ = static_cast<int>(channels);
    actual_format_ = config_.format;
    actual_period_size_ = static_cast<int>(actual_period_size);
    actual_periods_ = static_cast<int>(actual_periods);


    return true;


}

bool AlsaAudioSource::configureSwParams()
{
    auto* pcm = reinterpret_cast<snd_pcm_t*>(handle_);
    if(!pcm)
    {
        return false;
    }
    snd_pcm_sw_params_t* sw_params = nullptr;
    snd_pcm_sw_params_alloca(&sw_params);

    int ret = snd_pcm_sw_params_current(pcm, sw_params);
    if (ret < 0) {
        RKCAM_LOGE("snd_pcm_sw_params_current failed: %s",
                   alsaErrorString(ret));
        return false;
    }

    /*
     * avail_min = 一个 period。
     * 表示至少有一个 period 数据可读时再唤醒。
     */
    ret = snd_pcm_sw_params_set_avail_min(
        pcm,
        sw_params,
        static_cast<snd_pcm_uframes_t>(actual_period_size_)
    );
    if (ret < 0) {
        RKCAM_LOGE("snd_pcm_sw_params_set_avail_min failed: %s",
                   alsaErrorString(ret));
        return false;
    }

    /*
     * capture 场景下 start_threshold 设 1 比较常见：
     *这个参数定义了：“我要存够多少数据，才把状态机从‘待命’切换到‘正在录制’（RUNNING）？”
     *一旦切换到 RUNNING 之后，这个参数就彻底失效了。
     */
    ret = snd_pcm_sw_params_set_start_threshold(
        pcm,
        sw_params,
        1
    );
    if (ret < 0) {
        RKCAM_LOGE("snd_pcm_sw_params_set_start_threshold failed: %s",
                   alsaErrorString(ret));
        return false;
    }

    ret = snd_pcm_sw_params_set_tstamp_mode(
        pcm,
        sw_params,
        SND_PCM_TSTAMP_ENABLE
    );
    if (ret < 0) {
        RKCAM_LOGE("snd_pcm_sw_params_set_tstamp_mode failed: %s",
                alsaErrorString(ret));
        return false;
    }

#ifdef SND_PCM_TSTAMP_TYPE_MONOTONIC
    ret = snd_pcm_params_set_tstamp_type(
        pcm,
        sw_params,
        SND_PCM_TSTAMP_TYPE_MONOTONIC
    );
    if (ret < 0) {
        RKCAM_LOGE("snd_pcm_sw_params_set_tstamp_type MONOTONIC failed: %s",
                alsaErrorString(ret));
        /*
        * 这里不一定要 return false。
        * 有些老 ALSA/驱动可能不支持 tstamp_type。
        * 可以继续跑，后面 timestamp 查询失败再 fallback。
        */
    }

#endif


    ret = snd_pcm_sw_params(pcm, sw_params);
    if (ret < 0) {
        RKCAM_LOGE("snd_pcm_sw_params apply failed: %s",
                   alsaErrorString(ret));
        return false;
    }

    return true;

}

bool AlsaAudioSource::start()
{
    if (!opened_) {
        if (!open()) {
            return false;
        }
    }
    if(running_)
    {
        return true;
    }

    auto* pcm = reinterpret_cast<snd_pcm_t*>(handle_);
    if (!pcm) {
        return false;
    }

    const int ret = snd_pcm_prepare(pcm);
    if (ret < 0) {
        RKCAM_LOGE("AlsaAudioSource start prepare failed: %s",
                   alsaErrorString(ret));
        return false;
    }

    running_ = true;

    RKCAM_LOGI("AlsaAudioSource started: device=%s",
               config_.device.c_str());

    return true;

}

bool AlsaAudioSource::readFrame(AudioSourceFrame& frame)
{
    frame = AudioSourceFrame{};
    if (!running_ || !handle_) {
        return false;
    }

    const int target_frames = actual_period_size_;
    const int frame_bytes = bytesPerFrame();
    const int target_bytes = frameBufferBytes(target_frames);
    if (target_frames <= 0 ||
        frame_bytes <= 0 ||
        target_bytes <= 0) {
        RKCAM_LOGE("AlsaAudioSource invalid read size: target_frames=%d frame_bytes=%d target_bytes=%d",
                   target_frames,
                   frame_bytes,
                   target_bytes);
        return false;
    }
    auto buffer = std::make_shared<AudioBuffer>();
    buffer->data.resize(static_cast<size_t>(target_bytes));

    int total_frames = 0;

    while(total_frames < target_frames)
    {
        const int remain_frames = target_frames - total_frames;
        uint8_t* dst = buffer->data.data() + static_cast<size_t>(total_frames * frame_bytes);
        snd_pcm_sframes_t ret = snd_pcm_readi(
            reinterpret_cast<snd_pcm_t*>(handle_),
            dst,
            static_cast<snd_pcm_uframes_t>(remain_frames)
        );
        if (ret < 0) {
            if (!running_) {
                return false;
            }
            if (!recoverFromReadError(static_cast<int>(ret))) {
                return false;
            }
            continue;
        }
        if (ret == 0) {
            continue;
        }
        
        total_frames +=static_cast<int>(ret);
        if (config_.allow_partial_read) {
            break;
        }
    }
    if (total_frames <= 0) {
        return false;
    }
    const int valid_bytes = frameBufferBytes(total_frames);
    if (valid_bytes <= 0) {
        return false;
    }

    if(valid_bytes < target_bytes)
    {
        buffer->data.resize(static_cast<size_t>(valid_bytes));
    }
    const int64_t duration_us = audioDurationUs(total_frames, actual_sample_rate_);

    /*
     * readi 返回时，这一段音频已经采到了。
     * 这里用 now - duration 估算第一个 sample 的时间。
     *
     * 后面可以升级成 ALSA hw timestamp。
     */
    int64_t pts_us = 0;
    if(!getAlsaCapturePtsUs(total_frames, pts_us))
    {
        /*
        * fallback：
        * ALSA timestamp 不可用时，退回手动估算。
        */
        pts_us = nowUs() - duration_us;
    }


    frame.stream_id = config_.stream_id;
    frame.sample_rate = actual_sample_rate_;
    frame.channels = actual_channels_;
    frame.format = actual_format_;
    frame.nb_samples = total_frames;
    frame.pts_us = pts_us;
    frame.duration_us = duration_us;
    frame.frame_id = frame_id_++;
    frame.buffer = std::move(buffer);

    return true;
}

void AlsaAudioSource::releaseFrame(AudioSourceFrame& frame)
{
    /*
     * readi 模式下，AudioFrame 自己持有 vector 内存。
     * 不需要额外释放。
     */
    frame = AudioSourceFrame{};
}

void AlsaAudioSource::stop()
{
    if (!running_ && !handle_) {
        return;
    }

    running_ = false;

    if(handle_)
    {
        /*
         * capture stop 用 drop，立即停止。
         * drain 更适合 playback。
         */
        snd_pcm_drop(reinterpret_cast<snd_pcm_t*>(handle_));
    }
    RKCAM_LOGI("AlsaAudioSource stopped");
}

void AlsaAudioSource::close()
{
    if(running_)
    {
        stop();
    }
    if(handle_)
    {
        
        snd_pcm_close(reinterpret_cast<snd_pcm_t*>(handle_));
        handle_ = nullptr;
    }

    opened_ = false;
    running_ = false;
    RKCAM_LOGI("AlsaAudioSource closed");
}


bool AlsaAudioSource::recoverFromReadError(int err)
{
    auto* pcm = reinterpret_cast<snd_pcm_t*>(handle_);
    if (!pcm) {
        return false;
    }

    RKCAM_LOGE("snd_pcm_readi failed: %s",
               alsaErrorString(err));
    
    const int ret = snd_pcm_recover(pcm, err, 1);
    if (ret < 0) {
        RKCAM_LOGE("snd_pcm_recover failed: err=%s recover=%s",
                   alsaErrorString(err),
                   alsaErrorString(ret));
        return false;
    }

    return true;
}


int AlsaAudioSource::bytesPerFrame() const
{
    const int bytes_per_sample = audioSampleFormatBytes(actual_format_);
    if (bytes_per_sample <= 0 || actual_channels_ <= 0) {
        return 0;
    }
    return bytes_per_sample * actual_channels_;

}

int AlsaAudioSource::frameBufferBytes(int nb_samples) const
{
    if(nb_samples <= 0)
    {
        return 0;
    }
    return bytesPerFrame() * nb_samples;
}

int64_t AlsaAudioSource::nowUs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000LL + static_cast<int64_t>(ts.tv_nsec) / 1000LL;
}

bool AlsaAudioSource::getAlsaCapturePtsUs(int read_frames, int64_t& pts_us) const
{
// 时间轴（从左到右递增）---> ---> ---> ---> ---> ---> ---> ---> ---> ---> --->
// ==========================================================================
//  采样点进入麦克风时刻                    状态快照时刻 (htstamp)
//        │                                                │
//        ▼                                                ▼
//  [音频包首采样] ... [音频包尾采样] │ [缓冲区中尚未读走的数据]
//  └─── 你刚读走的 read_frames ───┘ └─── 缓冲区积压的 delay ───┘
//  └─────────────────── 跨越的总帧数 pending_frames ──────────┘

    pts_us = 0;

    if (!handle_ || read_frames <= 0 || actual_sample_rate_ <= 0) {
        return false;
    }

    auto* pcm = reinterpret_cast<snd_pcm_t*>(handle_);
    snd_pcm_status_t* status = nullptr;
    snd_pcm_status_alloca(&status);

    const int ret = snd_pcm_status(pcm, status);
    if (ret < 0) {
        RKCAM_LOGE("snd_pcm_status failed: %s",
                   snd_strerror(ret));
        return false;
    }

    snd_htimestamp_t ht{};
    snd_pcm_status_get_htstamp(status, &ht);
    if(ht.tv_sec == 0 && ht.tv_nsec == 0)
    {
        RKCAM_LOGE("snd_pcm_status_get_htstamp invalid: 0");
        return false;
    }

    snd_pcm_sframes_t delay = snd_pcm_status_get_delay(status);
    if(delay < 0)
    {
        delay = 0;
    }

    const int64_t ht_us = static_cast<int64_t>(ht.tv_sec) * 1000000LL + static_cast<int64_t>(ht.tv_nsec) / 1000LL;

    /*
     * capture 场景：
     *
     * readi() 已经读出了 read_frames。
     * snd_pcm_status_get_delay() 表示当前 ALSA capture buffer 里
     * 还积压了多少 frames 没被读走。
     *
     * 所以刚刚读出的这一段音频的第一个 sample 时间大约是：
     *
     *   htstamp - (delay + read_frames) 的音频时长
     */
    const int64_t pending_frames = static_cast<int64_t>(delay) + read_frames;
    const int64_t pending_us = pending_frames * 1000000LL / actual_sample_rate_;

    pts_us = ht_us - pending_us;

    return pts_us > 0;
}


}