#include "rkcam/pipeline/wav_save_stage.hpp"

#include "rkcam/core/log.hpp"

#include <cerrno>
#include <cstring>
namespace rkcam{

namespace{
// struct WavHeader {
//     /* ==================== 1. RIFF 块头 (12 字节) ==================== */
//     char     riff_id[4];        // "RIFF" 标识符 (大端 ASCII)
//     uint32_t total_size;        // 文件总大小 - 8 字节 (等于 36 + data_size)
//     char     wave_id[4];        // "WAVE" 格式标识符 (大端 ASCII)

//     /* ==================== 2. fmt 子块 (24 字节) ==================== */
//     char     fmt_id[4];         // "fmt " 标识符 (注意末尾有空格，大端 ASCII)
//     uint32_t fmt_size;          // fmt 子块的大小，PCM 格式固定为 16 字节
//     uint16_t audio_format;      // 音频格式标记：1 代表 PCM 线性整数 (对应你代码中的 format_)
//                                 //              3 代表 IEEE 浮点数
//     uint16_t channels;          // 声道数：1 = 单声道，2 = 双声道 (对应 channels_)
//     uint32_t sample_rate;       // 采样率：例如 16000, 44100, 48000 (对应 sample_rate_)
//     uint32_t byte_rate;         // 每秒字节率 = sample_rate * block_align (对应 byte_rate_)
//     uint16_t block_align;       // 数据块对齐 = channels * bits_per_sample / 8 (对应 block_align_)
//     uint16_t bits_per_sample;   // 位深度 / 采样位数：例如 8, 16, 24, 32 (对应 bits_per_sample_)

//     /* ==================== 3. data 子块头 (8 字节) ==================== */
//     char     data_id[4];        // "data" 标识符 (大端 ASCII)
//     uint32_t data_size;         // 接下来纯原始 PCM 音频数据的总字节数
// };
#pragma pack(push, 1)
// 默认情况下，为了让 CPU 读取内存效率最高，C/C++ 编译器会自动在结构体的成员之间插入一些无意义的填充字节（Padding），让各个变量的内存地址对齐到 2、4 或 8 的倍数上
//#pragma pack(push, 1)（MSVC / GCC 通用）或 __attribute__((packed))（GCC / Clang 特有）：告诉编译器：“取消所有自动填充！严格按照 1 字节紧凑排列所有成员变量！”，保证 sizeof(WavHeader) == 44
struct WavHeader{

    char riff_if[4] = {'R', 'I', 'F', 'F'};
    uint32_t riff_size = 0;
    
    char wave_id[4] = {'W', 'A', 'V', 'E'};

    char fmt_id[4] = {'f', 'm', 't', ' '};
    uint32_t fmt_size = 16;

    uint16_t audio_format = 1;
    uint16_t num_channels = 0;
    uint32_t sample_rate = 0;
    uint32_t byte_rate = 0;
    uint16_t block_align = 0;
    uint16_t bits_per_sample = 0;
    
    char data_id[4] = {'d', 'a', 't', 'a'};
    uint32_t data_size = 0;
};
#pragma pack(pop)
static_assert(sizeof(WavHeader) == 44, "WavHeader must be 44 bytes");

// 小端（Little-Endian）：低位字节存放在低地址（如数字 0x12345678 存为 78 56 34 12）
// 大端（Big-Endian）：高位字节存放在低地址（如数字 0x12345678 存为 12 34 56 78）
// WAV 规范硬性规定：文件头里的所有数字必须用小端格式存放
// RK3568 架构：你的芯片是 ARM64（RK3568），运行的 Linux 系统默认就是小端模式
// 如果未来这段代码运行在某些强制大端（Big-Endian）的处理器上，你直接 fwrite 结构体，存进去的字节序就会是反的，WAV 就会损坏。因此最严谨（但代码略繁琐）的做法是不写结构体，而是用 writeLe32() 这种函数显式把数字强转成小端一个个字节写入
} //namespace



WavSaveStage::WavSaveStage(
    const WavSaveStageConfig& config,
    BlockingQueue<PipelineAudioFrame>& input_queue)
    : config_(config),
      input_queue_(input_queue)
{
}

WavSaveStage::~WavSaveStage()
{
    stop();
}

bool WavSaveStage::start()
{
    if (running_) {
        return true;
    }

    if (config_.output_path.empty()) {
        RKCAM_LOGE("[%s] start failed: output_path is empty",
                   config_.stage_name.c_str());
        return false;
    }

    saved_frames_ = 0;
    failed_frames_ = 0;
    data_bytes_ = 0;

    sample_rate_ = 0;
    channels_ = 0;
    format_ = AudioSampleFormat::Unknown;
    bits_per_sample_ = 0;
    block_align_ = 0;
    byte_rate_ = 0;

    running_ = true;
    thread_ = std::thread(&WavSaveStage::threadLoop, this);

    RKCAM_LOGI("[%s] WavSaveStage started, output=%s",
               config_.stage_name.c_str(),
               config_.output_path.c_str());

    return true;

}

void WavSaveStage::stop()
{
    if(!running_ && !thread_.joinable())
    {
        return;
    }
    input_queue_.stop();
    if(thread_.joinable())
    {
        thread_.join();
    }

    closeFile();

    running_ = false;

    RKCAM_LOGI("[%s] WavSaveStage stopped, saved_frames=%d failed_frames=%d data_bytes=%llu",
               config_.stage_name.c_str(),
               saved_frames_,
               failed_frames_,
               static_cast<unsigned long long>(data_bytes_));

}

void WavSaveStage::threadLoop()
{
    while(true)
    {
        PipelineAudioFrame frame;
        if(!input_queue_.pop(frame))
        {
            if(input_queue_.stopped())
            {
                break;
            }
            continue;
        }

        if(frame.empty())
        {
            ++failed_frames_;
            continue;
        }

        if(!fp_)
        {
            if(!openFileFromFrame(frame))
            {
                ++failed_frames_;
                break;
            }
        }
        if(!validateFrame(frame))
        {
            ++failed_frames_;
            if (config_.strict_format) {
                RKCAM_LOGE("[%s] strict format mismatch, exit",
                           config_.stage_name.c_str());
                break;
            }

            continue;
        }

        if(!writeFrame(frame))
        {
            ++failed_frames_;
            RKCAM_LOGE("[%s] writeFrame failed, frame_id=%lld size=%zu",
                       config_.stage_name.c_str(),
                       static_cast<long long>(frame.frame_id),
                       frame.size());
            break;
        }

        ++saved_frames_;
        if(config_.log_interval > 0 && saved_frames_ % config_.log_interval == 0)
        {
            RKCAM_LOGI("[%s] saved_audio_frames=%d stream=%s samples=%d pts=%lld duration=%lld size=%zu data_bytes=%llu",
                       config_.stage_name.c_str(),
                       saved_frames_,
                       frame.stream_id.c_str(),
                       frame.nb_samples,
                       static_cast<long long>(frame.pts_us),
                       static_cast<long long>(frame.duration_us),
                       frame.size(),
                       static_cast<unsigned long long>(data_bytes_));
        }
        if(config_.max_frames > 0 && saved_frames_ >= config_.max_frames)
        {
            RKCAM_LOGI("[%s] reached max_frames=%d",
                       config_.stage_name.c_str(),
                       config_.max_frames);
            break;
        }

    }
    running_ = false;
    RKCAM_LOGI("[%s] WavSaveStage thread exit, saved_frames=%d failed_frames=%d data_bytes=%llu",
               config_.stage_name.c_str(),
               saved_frames_,
               failed_frames_,
               static_cast<unsigned long long>(data_bytes_));
}


bool WavSaveStage::openFileFromFrame(const PipelineAudioFrame& frame)
{
    if (frame.sample_rate <= 0 ||
        frame.channels <= 0 ||
        frame.format == AudioSampleFormat::Unknown) {
        RKCAM_LOGE("[%s] invalid first audio frame: rate=%d channels=%d format=%d",
                   config_.stage_name.c_str(),
                   frame.sample_rate,
                   frame.channels,
                   static_cast<int>(frame.format));
        return false;
    }


    const int bits = wavBitsPerSample(frame.format);
    if (bits <= 0) {
        RKCAM_LOGE("[%s] unsupported WAV format=%d",
                   config_.stage_name.c_str(),
                   static_cast<int>(frame.format));
        return false;
    }

    sample_rate_ = frame.sample_rate;
    channels_ = frame.channels;
    format_ = frame.format;
    bits_per_sample_ = bits;
    block_align_ = channels_ * bits_per_sample_ / 8;
    byte_rate_ = sample_rate_ * block_align_;

    fp_ = std::fopen(config_.output_path.c_str(), "wb+");
    if (!fp_) {
        RKCAM_LOGE("[%s] fopen failed: path=%s err=%s",
                   config_.stage_name.c_str(),
                   config_.output_path.c_str(),
                   std::strerror(errno));
        return false;
    }
    /*
     * 先写占位 header。
     * 结束时根据 data_bytes_ 回填真实大小。
     */
    if(!writeWavHeader(0))
    {
        RKCAM_LOGE("[%s] write placeholder WAV header failed",
                   config_.stage_name.c_str());
        closeFile();
        return false;
    }


    RKCAM_LOGI("[%s] WAV opened: path=%s rate=%d channels=%d format=%s bits=%d block_align=%d byte_rate=%d",
               config_.stage_name.c_str(),
               config_.output_path.c_str(),
               sample_rate_,
               channels_,
               audioSampleFormatToString(format_),
               bits_per_sample_,
               block_align_,
               byte_rate_);
    return true;
}

bool WavSaveStage::validateFrame(const PipelineAudioFrame& frame) const{
    if (frame.sample_rate != sample_rate_ ||
        frame.channels != channels_ ||
        frame.format != format_) {
        RKCAM_LOGE("[%s] audio format mismatch: "
                   "frame rate=%d channels=%d format=%d, "
                   "wav rate=%d channels=%d format=%d",
                   config_.stage_name.c_str(),
                   frame.sample_rate,
                   frame.channels,
                   static_cast<int>(frame.format),
                   sample_rate_,
                   channels_,
                   static_cast<int>(format_));
        return false;
    }
    if (!frame.data() || frame.size() == 0) {
        RKCAM_LOGE("[%s] invalid audio frame data",
                   config_.stage_name.c_str());
        return false;
    }

    if (block_align_ <= 0) {
        return false;
    }
    if (frame.size() % static_cast<size_t>(block_align_) != 0) {
        RKCAM_LOGE("[%s] frame size is not aligned: size=%zu block_align=%d",
                   config_.stage_name.c_str(),
                   frame.size(),
                   block_align_);
        return false;
    }

    return true;
}

bool WavSaveStage::writeFrame(const PipelineAudioFrame& frame)
{
    if (!fp_) {
        return false;
    }

    /*
     * classic WAV RIFF 的 data size 是 uint32。
     * 第一版不支持超过 4GB 的 WAV。
     */

    if(data_bytes_ + frame.size() > 0xffffffffULL)
    {
        RKCAM_LOGE("[%s] WAV file too large for RIFF: data_bytes=%llu next=%zu",
                   config_.stage_name.c_str(),
                   static_cast<unsigned long long>(data_bytes_),
                   frame.size());
        return false;
    }
    const size_t written = std::fwrite(frame.data(), 1, frame.size(), fp_);
    if (written != frame.size()) {
        RKCAM_LOGE("[%s] fwrite failed: written=%zu expected=%zu err=%s",
                   config_.stage_name.c_str(),
                   written,
                   frame.size(),
                   std::strerror(errno));
        return false;
    }

    data_bytes_ += frame.size();

    return true;

}


bool WavSaveStage::writeWavHeader(uint32_t data_size){
    if(!fp_)
    {
        return false;
    }

    const uint32_t riff_size = 36u + data_size;

    /*
     * RIFF header:
     *
     * 0  - 3   "RIFF"
     * 4  - 7   file size - 8
     * 8  - 11  "WAVE"
     * 12 - 15  "fmt "
     * 16 - 19  fmt chunk size = 16
     * 20 - 21  audio format = 1 PCM
     * 22 - 23  channels
     * 24 - 27  sample rate
     * 28 - 31  byte rate
     * 32 - 33  block align
     * 34 - 35  bits per sample
     * 36 - 39  "data"
     * 40 - 43  data size
     */
    //手动写入小端的wavhead最安全
    return writeBytes(fp_, "RIFF", 4) &&
           writeLe32(fp_, riff_size) &&
           writeBytes(fp_, "WAVE", 4) &&
           writeBytes(fp_, "fmt ", 4) &&
           writeLe32(fp_, 16) &&
           writeLe16(fp_, 1) &&
           writeLe16(fp_, static_cast<uint16_t>(channels_)) &&
           writeLe32(fp_, static_cast<uint32_t>(sample_rate_)) &&
           writeLe32(fp_, static_cast<uint32_t>(byte_rate_)) &&
           writeLe16(fp_, static_cast<uint32_t>(block_align_)) &&
           writeLe16(fp_, static_cast<uint16_t>(bits_per_sample_)) &&
           writeBytes(fp_, "data", 4) &&
           writeLe32(fp_, data_size);

}

bool WavSaveStage::updateWavHeader()
{
    if (!fp_) {
        return true;
    }
    if (data_bytes_ > 0xffffffffULL) {
        RKCAM_LOGE("[%s] data_bytes too large for WAV header: %llu",
                   config_.stage_name.c_str(),
                   static_cast<unsigned long long>(data_bytes_));
        return false;
    }

    if(std::fflush(fp_) != 0)
    {
        RKCAM_LOGE("[%s] fflush before update header failed: %s",
                   config_.stage_name.c_str(),
                   std::strerror(errno));
        return false;
    }

    if(std::fseek(fp_, 0, SEEK_SET) != 0)
    {
        RKCAM_LOGE("[%s] fseek header failed: %s",
                   config_.stage_name.c_str(),
                   std::strerror(errno));
        return false;
    }

    if(!writeWavHeader(static_cast<uint32_t>(data_bytes_)))
    {
        return false;
    }

    if(std::fflush(fp_) != 0)
    {
        RKCAM_LOGE("[%s] fflush after update header failed: %s",
                   config_.stage_name.c_str(),
                   std::strerror(errno));
        return false;
    }

    RKCAM_LOGI("[%s] WAV header updated: data_bytes=%llu",
               config_.stage_name.c_str(),
               static_cast<unsigned long long>(data_bytes_));

    return true;
}

void WavSaveStage::closeFile()
{
    if(!fp_)
    {
        return;
    }

    updateWavHeader();

    std::fclose(fp_);
    fp_ = nullptr;
    RKCAM_LOGI("[%s] WAV closed: path=%s data_bytes=%llu",
               config_.stage_name.c_str(),
               config_.output_path.c_str(),
               static_cast<unsigned long long>(data_bytes_));
}


int WavSaveStage::wavBitsPerSample(AudioSampleFormat format)
{
    switch (format){
        case AudioSampleFormat::S16LE:
            return 16;
        case AudioSampleFormat::S32LE:
            return 32;
        /*
        * 你的 ALSA 虽然支持 S24_LE，但很多平台的 S24_LE 是
        * 24-bit valid in 32-bit container。
        * 第一版先不写，避免 WAV header 和实际存储不一致。
        */
        case AudioSampleFormat::S24LE:
            return 0;
        default:
            return 0;
    }
}


bool WavSaveStage::writeLe16(std::FILE* fp, uint16_t value)
{
    const uint8_t data[2] = {
        static_cast<uint8_t>(value & 0xff),
        static_cast<uint8_t>((value >> 8) & 0xff)
    };
    return writeBytes(fp, data, sizeof(data));
}

bool WavSaveStage::writeLe32(std::FILE* fp, uint32_t value)
{
    const uint8_t data[4] = {
        static_cast<uint8_t>(value & 0xff),
        static_cast<uint8_t>((value >> 8) & 0xff),
        static_cast<uint8_t>((value >> 16) & 0xff),
        static_cast<uint8_t>((value >> 24) & 0xff),
    };
    return writeBytes(fp, data, sizeof(data));
}

bool WavSaveStage::writeBytes(std::FILE* fp, const void* data, size_t size)
{
    if(!fp || !data || size == 0)
    {
        return false;
    }
    return std::fwrite(data, 1, size, fp) == size;
}

}//namespace rkcam