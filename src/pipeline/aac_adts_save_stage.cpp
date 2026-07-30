#include "rkcam/pipeline/aac_adts_save_stage.hpp"

#include "rkcam/core/log.hpp"

#include <cerrno>
#include <cstring>

namespace rkcam{


AacAdtsSaveStage::AacAdtsSaveStage(
    const AacAdtsSaveStageConfig& config,
    BlockingQueue<EncodedPacket>& input_queue)
    : config_(config),
      input_queue_(input_queue)
{
}

AacAdtsSaveStage::~AacAdtsSaveStage()
{
    stop();
}


bool AacAdtsSaveStage::start()
{
    if(running_)
    {
        return true;
    }
    if(config_.stage_name.empty())
    {
        config_.stage_name = "aac_adts_save";
    }
    if(config_.output_path.empty())
    {
        RKCAM_LOGE("[%s] start failed: output_path is empty",
                   config_.stage_name.c_str());
        return false;
    }
    if (config_.sample_rate <= 0 ||
        config_.channels <= 0 ||
        config_.channels > 7) {
        RKCAM_LOGE("[%s] invalid config: sample_rate=%d channels=%d",
                   config_.stage_name.c_str(),
                   config_.sample_rate,
                   config_.channels);
        return false;
    }

    if(aacSampleRateIndex(config_.sample_rate) < 0)
    {
        RKCAM_LOGE("[%s] unsupported ADTS sample_rate=%d",
                   config_.stage_name.c_str(),
                   config_.sample_rate);
        return false;
    }

    if(!openFile())
    {
        return false;
    }
    saved_packets_ = 0;
    failed_packets_ = 0;
    written_bytes_ = 0;

    running_ = true;
    thread_ = std::thread(&AacAdtsSaveStage::threadLoop, this);

    RKCAM_LOGI("[%s] AacAdtsSaveStage started: output=%s rate=%d channels=%d",
               config_.stage_name.c_str(),
               config_.output_path.c_str(),
               config_.sample_rate,
               config_.channels);

    return true;
}
void AacAdtsSaveStage::stop()
{
    if (!running_ && !thread_.joinable()) {
        return;
    }
    input_queue_.stop();
    if(thread_.joinable())
    {
        thread_.join();
    }
    closeFile();
    running_ = false;

    RKCAM_LOGI("[%s] AacAdtsSaveStage stopped, saved_packets=%d failed_packets=%d written_bytes=%llu",
               config_.stage_name.c_str(),
               saved_packets_,
               failed_packets_,
               static_cast<unsigned long long>(written_bytes_));
}


void AacAdtsSaveStage::threadLoop()
{
    while(true)
    {
        EncodedPacket packet;
        if(!input_queue_.pop(packet))
        {
            if(input_queue_.stopped())
            {
                break;
            }
            continue;
        }
        if(packet.eos)
        {
            RKCAM_LOGI("[%s] got eos packet",
                       config_.stage_name.c_str());
            break;
        }
        if (!validatePacket(packet)) {
            ++failed_packets_;

            if (config_.strict_packet) {
                RKCAM_LOGE("[%s] invalid AAC packet, failed_packets=%d",
                           config_.stage_name.c_str(),
                           failed_packets_);
            }

            continue;
        }
        if(!writePacket(packet))
        {
            ++failed_packets_;

            RKCAM_LOGE("[%s] writePacket failed, pts=%lld size=%zu failed_packets=%d",
                       config_.stage_name.c_str(),
                       static_cast<long long>(packet.pts_us),
                       packet.size(),
                       failed_packets_);
            break;
        }
        ++saved_packets_;
        if (config_.log_interval > 0 &&
            saved_packets_ % config_.log_interval == 0) {
            RKCAM_LOGI("[%s] saved_aac_packets=%d stream=%s pts=%lld duration=%lld payload=%zu total=%llu",
                       config_.stage_name.c_str(),
                       saved_packets_,
                       packet.stream_id.c_str(),
                       static_cast<long long>(packet.pts_us),
                       static_cast<long long>(packet.duration_us),
                       packet.size(),
                       static_cast<unsigned long long>(written_bytes_));
        }

        if (config_.max_packets > 0 &&
            saved_packets_ >= config_.max_packets) {
            RKCAM_LOGI("[%s] reached max_packets=%d",
                       config_.stage_name.c_str(),
                       config_.max_packets);
            break;
        }
    }
    running_ = false;
    RKCAM_LOGI("[%s] AacAdtsSaveStage thread exit, saved_packets=%d failed_packets=%d written_bytes=%llu",
               config_.stage_name.c_str(),
               saved_packets_,
               failed_packets_,
               static_cast<unsigned long long>(written_bytes_));
}

bool AacAdtsSaveStage::openFile()
{
    fp_ = fopen(config_.output_path.c_str(), "wb");
    if (!fp_) {
        RKCAM_LOGE("[%s] fopen failed: path=%s err=%s",
                   config_.stage_name.c_str(),
                   config_.output_path.c_str(),
                   std::strerror(errno));
        return false;
    }

    return true;
}
void AacAdtsSaveStage::closeFile()
{
    if(!fp_)
    {
        return;
    }
    std::fflush(fp_);
    std::fclose(fp_);
    fp_ = nullptr;

    RKCAM_LOGI("[%s] AAC ADTS file closed: path=%s written_bytes=%llu",
               config_.stage_name.c_str(),
               config_.output_path.c_str(),
               static_cast<unsigned long long>(written_bytes_));

}

bool AacAdtsSaveStage::validatePacket(const EncodedPacket& packet) const
{
    if (packet.media_type != MediaType::Audio ||
        packet.codec != CodecType::AAC) {
        if (config_.strict_packet) {
            RKCAM_LOGE("[%s] unsupported packet: media=%d codec=%d",
                       config_.stage_name.c_str(),
                       static_cast<int>(packet.media_type),
                       static_cast<int>(packet.codec));
        }

        return false;
    }

    if (packet.empty()) {
        if (config_.strict_packet) {
            RKCAM_LOGE("[%s] empty AAC packet",
                       config_.stage_name.c_str());
        }

        return false;
    }

    return true;
}


bool AacAdtsSaveStage::writePacket(const EncodedPacket& packet)
{
    if(!fp_)
    {
        return false;
    }

    uint8_t header[7]{};
    if(!makeAdtsHeader(packet.size(), header))
    {
        return false;
    }

    const size_t header_written = std::fwrite(header, 1, sizeof(header), fp_);
    if (header_written != sizeof(header)) {
        RKCAM_LOGE("[%s] fwrite ADTS header failed: written=%zu expected=%zu err=%s",
                   config_.stage_name.c_str(),
                   header_written,
                   sizeof(header),
                   std::strerror(errno));
        return false;
    }

    const size_t payload_written = std::fwrite(packet.data(), 1, packet.size(), fp_);
    if (payload_written != packet.size()) {
        RKCAM_LOGE("[%s] fwrite AAC payload failed: written=%zu expected=%zu err=%s",
                   config_.stage_name.c_str(),
                   payload_written,
                   packet.size(),
                   std::strerror(errno));
        return false;
    }

    written_bytes_ += sizeof(header) + packet.size();

    return true;

}


bool AacAdtsSaveStage::makeAdtsHeader(size_t payload_size, uint8_t header[7]) const
{
    if(!header)
    {
        return false;
    }

    const int sample_index = aacSampleRateIndex(config_.sample_rate);
    if (sample_index < 0) {
        return false;
    }

    if (config_.channels <= 0 || config_.channels > 7) {
        return false;
    }
    /*
     * ADTS frame length 是 header + AAC raw payload。
     * 13 bit，最大 8191。
     */
     const size_t frame_length = payload_size + 7;
    if (frame_length > 0x1fff) {
        RKCAM_LOGE("[%s] ADTS frame too large: payload=%zu frame_length=%zu",
                   config_.stage_name.c_str(),
                   payload_size,
                   frame_length);
        return false;
    }
    /*
    * 当前 AacEncoder 固定输出 AAC-LC:
    *   audioObjectType = 2
    *
    * ADTS header 里的 profile 字段写的是:
    *   audioObjectType - 1
    *
    * 所以 AAC-LC 对应:
    *   profile = 1
    *
    * 如果以后支持其他 AAC profile，需要同时修改:
    *   1. AacEncoder profile
    *   2. AudioSpecificConfig
    *   3. ADTS profile
    */

    const int profile = 1;

    const int channels = config_.channels;
    /*
     *   syncword        1111 1111 1111
     *   MPEG-4          ID=0
     *   layer           00
     *   protection_absent=1
     */
    header[0] = 0xff;
    header[1] = 0xf1;

    header[2] = static_cast<uint8_t>(
        ((profile & 0x03) << 6) |
        ((sample_index & 0x0f) << 2) |
        ((channels >> 2) & 0x01) 
    );

    // Bit 7   Bit 6 │ Bit 5   Bit 4   Bit 3   Bit 2 │ Bit 1   Bit 0
    // ──────────────┼───────────────────────────────┼───────────────
    // channels低2位 │          0 0 0 0 (4b)         │frame_length高2位
    // Bit 5 ~ 2 (4 位)：Original / Home / Copyright 等控制位（代码里省略了，全填 0）
    header[3] = 
    static_cast<uint8_t>(
        ((channels & 0x03) << 6) |
        ((frame_length >> 11) & 0x03)
    );
    header[4] = static_cast<uint8_t>(
        (frame_length >> 3) & 0xff
    );

    header[5] = static_cast<uint8_t>(
        ((frame_length & 0x07) << 5) |
        0x1f
    );

    header[6] = 0xfc;

    return true;

}

int AacAdtsSaveStage::aacSampleRateIndex(int sample_rate)
{
    switch (sample_rate) {
    case 96000: return 0;
    case 88200: return 1;
    case 64000: return 2;
    case 48000: return 3;
    case 44100: return 4;
    case 32000: return 5;
    case 24000: return 6;
    case 22050: return 7;
    case 16000: return 8;
    case 12000: return 9;
    case 11025: return 10;
    case 8000:  return 11;
    case 7350:  return 12;
    default:    return -1;
    }
}

}