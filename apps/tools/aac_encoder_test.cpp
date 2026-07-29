#include "rkcam/media/aac_encoder.hpp"
#include "rkcam/audio/audio_types.hpp"
#include "rkcam/core/log.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>


namespace{

struct WavInfo{

    int audio_format = 0;
    int channels = 0;
    int sample_rate = 0;
    int byte_rate = 0;
    int block_align = 0;
    int bits_per_sample = 0;

    long data_offset = 0;
    uint32_t data_size = 0;
};

bool readBytes(std::FILE* fp, void* data, size_t size)
{
    return std::fread(data, 1, size, fp) == size;
}

bool readLe16(std::FILE* fp, uint16_t& value)
{
    uint8_t b[2] {};
    if(!readBytes(fp, b, sizeof(b)))
    {
        return false;
    }

    value = static_cast<uint16_t>(b[0]) | static_cast<int16_t>(b[1] << 8);
    return true;

}

bool readLe32(std::FILE* fp, uint32_t& value)
{
    uint8_t b[4] {};
    if(!readBytes(fp, b, sizeof(b)))
    {
        return false;
    }

    value = static_cast<uint32_t>(b[0]) |
        (static_cast<uint32_t>(b[1] << 8)) |
        (static_cast<uint32_t>(b[2] << 16)) |
        (static_cast<uint32_t>(b[3] << 24));
    return true;

}



bool skipBytes(std::FILE* fp, uint32_t size)
{
    const long chunk_header_pos = std::ftell(fp);
    std::printf("skipBytes chunk offset =0x%lx size = %d\n",
                chunk_header_pos, size);

    if(std::fseek(fp, static_cast<long>(size), SEEK_CUR) != 0)  //SEEK_cUR 当前位置，偏移size
    {
        return false;
    }
    /*
     * wav header 的 chunk 都是 2 字节对齐。
     * chunk size 为奇数时，后面会补 1 字节 padding。
     */
    if(size & 1u)
    {
        if(std::fseek(fp, 1, SEEK_CUR) != 0)
        {
            return false;
        }
    }

    return true;

}


bool parseWav(const std::string& path, WavInfo& info)
{
    info = WavInfo{};

    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        std::fprintf(stderr, "fopen wav failed: %s err=%s\n",
                     path.c_str(),
                     std::strerror(errno));
        return false;
    }

    char riff[4] {};
    uint32_t riff_size = 0;
    char wave[4] {};

    if (!readBytes(fp, riff, 4) ||
        !readLe32(fp, riff_size) ||
        !readBytes(fp, wave, 4)) {
        std::fprintf(stderr, "invalid wav header\n");
        std::fclose(fp);
        return false;
    }

    if (std::memcmp(riff, "RIFF", 4) != 0 ||
        std::memcmp(wave, "WAVE", 4) != 0) {
        std::fprintf(stderr, "not RIFF/WAVE file\n");
        std::fclose(fp);
        return false;
    }

    bool got_fmt = false;
    bool got_data = false;

    while(!got_data)
    {
        char chunk_id[4] {};
        uint32_t chunk_size = 0;
        if(!readBytes(fp, chunk_id, 4) || 
            !readLe32(fp, chunk_size))
        {
            break;
        }

        // const long chunk_header_pos = std::ftell(fp);
        // std::printf("chunk offset=0x%lx id=%.4s size=%u\n",
        //             chunk_header_pos,
        //             chunk_id,
        //             chunk_size);
        std::printf("chunk_id : %s", chunk_id);
        std::printf("chunk_size : %d\n", chunk_size);
        if(std::memcmp(chunk_id, "fmt ", 4) == 0)
        {
            uint16_t audio_format = 0;
            uint16_t channels = 0;
            uint32_t sample_rate = 0;
            uint32_t byte_rate = 0;
            uint16_t block_align = 0;
            uint16_t bits_per_sample = 0;

            if (chunk_size < 16) {
                std::fprintf(stderr, "invalid fmt chunk size=%u\n",
                             chunk_size);
                std::fclose(fp);
                return false;
            }

            if (!readLe16(fp, audio_format) ||
                !readLe16(fp, channels) ||
                !readLe32(fp, sample_rate) ||
                !readLe32(fp, byte_rate) ||
                !readLe16(fp, block_align) ||
                !readLe16(fp, bits_per_sample)) {
                std::fprintf(stderr, "read fmt chunk failed\n");
                std::fclose(fp);
                return false;
            }

            info.audio_format = audio_format;
            info.channels = channels;
            info.sample_rate = static_cast<int>(sample_rate);
            info.byte_rate = static_cast<int>(byte_rate);
            info.block_align = block_align;
            info.bits_per_sample = bits_per_sample;

            got_fmt = true;

            const uint32_t remain = chunk_size - 16;
            if (!skipBytes(fp, remain)) {
                std::fprintf(stderr, "skip fmt extra failed\n");
                std::fclose(fp);
                return false;
            }
        }
        else if(std::memcmp(chunk_id, "data", 4) == 0)
        {
            info.data_offset = std::ftell(fp);
            info.data_size = chunk_size;
            got_data = true;
            if (!skipBytes(fp, chunk_size)) {
                /*
                 * data 是最后一个 chunk 时，fseek 到 EOF 也可能失败。
                 * 这里不直接认为解析失败，后面会重新 fseek 到 data_offset。
                 */
            }
        }else {
            if (!skipBytes(fp, chunk_size)) {
                std::fprintf(stderr, "skip chunk %.4s failed\n", chunk_id);
                std::fclose(fp);
                return false;
            }
        }
    }
    std::fclose(fp);
    if (!got_fmt || !got_data) {
        if(!got_fmt)
        {
            std::fprintf(stderr, "wav missing fmt chunk\n");
        }
        if(!got_data)
        {
            std::fprintf(stderr, "wav missing data chunk\n");
        }
        return false;
    }

    if (info.audio_format != 1) {
        std::fprintf(stderr, "only PCM wav is supported, audio_format=%d\n",
                     info.audio_format);
        return false;
    }

    if (info.bits_per_sample != 16) {
        std::fprintf(stderr, "only S16_LE wav is supported, bits=%d\n",
                     info.bits_per_sample);
        return false;
    }

    if (info.channels <= 0 ||
        info.sample_rate <= 0 ||
        info.block_align <= 0 ||
        info.data_offset <= 0 ||
        info.data_size == 0) {
        std::fprintf(stderr,
                     "invalid wav info: channels=%d rate=%d block_align=%d data_offset=%ld data_size=%u\n",
                     info.channels,
                     info.sample_rate,
                     info.block_align,
                     info.data_offset,
                     info.data_size);
        return false;
    }

    return true;

}

int aacSampleRateIndex(int sample_rate)
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

bool makeAdtsHeader(int sample_rate, int channels, size_t payload_size, uint8_t header[7])
{
    const int sample_index = aacSampleRateIndex(sample_rate);
    if (sample_index < 0) {
        std::fprintf(stderr, "unsupported ADTS sample_rate=%d\n",
                     sample_rate);
        return false;
    }

    if (channels <= 0 || channels > 7) {
        std::fprintf(stderr, "unsupported ADTS channels=%d\n",
                     channels);
        return false;
    }

    const size_t frame_length = payload_size + 7;
    if (frame_length > 0x1fff) {
        std::fprintf(stderr, "ADTS frame too large: %zu\n",
                     frame_length);
        return false;
    }


    // // 1. 如果你在拼 MP4 / RTMP 的 AudioSpecificConfig (2 字节)
    // const int audio_object_type = 2; // AAC-LC
    // // 2. 如果你在拼纯 AAC 文件的 ADTS Header (7 字节)
    // int profile = 1; // AAC-LC (即 AOT - 1 = 2 - 1 = 1)

    const int profile = 1; // AAC LC - 1

    header[0] = 0xff;
    header[1] = 0xf1; // MPEG-4, layer 0, protection_absent 1
    header[2] = static_cast<uint8_t>(
        ((profile & 0x03) << 6) |
        ((sample_index & 0x0f) << 2) |
        ((channels >> 2) & 0x01)
    );


    header[3] = static_cast<uint8_t>(
        ((channels & 0x03) << 6) |
        ((frame_length >> 11) & 0x03));

    header[4] = static_cast<uint8_t>(
        (frame_length >> 3) & 0xff);

    header[5] = static_cast<uint8_t>(
        ((frame_length & 0x07) << 5) |
        0x1f);

    header[6] = 0xfc;
    
    return true;

}
bool writeAacAdtsPacket(
    std::FILE* fp,
    const rkcam::EncodedPacket& packet,
    int sample_rate,
    int channels)
{
    if (!fp || packet.empty()) {
        // std::printf("fp or packet empty\n");
        return false;
    }

    uint8_t adts[7] {};
    if (!makeAdtsHeader(sample_rate, channels, packet.size(), adts)) {
        return false;
    }

    if (std::fwrite(adts, 1, sizeof(adts), fp) != sizeof(adts)) {
        return false;
    }

    if (std::fwrite(packet.data(), 1, packet.size(), fp) != packet.size()) {
        return false;
    }

    return true;
}
int64_t audioSamplesToUs(int64_t samples, int sample_rate)
{
    if (samples <= 0 || sample_rate <= 0) {
        return 0;
    }

    return samples * 1000000LL / sample_rate;
}

} // namespace
int main(int argc, char** argv)
{
    const std::string input_wav =
        argc >= 2 ? argv[1] : "/userdata/rkcam/output/test_audio.wav";

    const std::string output_aac =
        argc >= 3 ? argv[2] : "/userdata/rkcam/output/test_audio.aac";

    const int bit_rate =
        argc >= 4 ? std::atoi(argv[3]) : 64000;

    std::printf("aac_encoder_test\n");
    std::printf("input_wav = %s\n", input_wav.c_str());
    std::printf("output_aac = %s\n", output_aac.c_str());
    std::printf("bit_rate = %d\n", bit_rate);

    WavInfo wav;
    if (!parseWav(input_wav, wav)) {
        std::fprintf(stderr, "parse wav failed\n");
        return 1;
    }

    std::printf("wav: rate=%d channels=%d bits=%d block_align=%d byte_rate=%d data_offset=%ld data_size=%u\n",
                wav.sample_rate,
                wav.channels,
                wav.bits_per_sample,
                wav.block_align,
                wav.byte_rate,
                wav.data_offset,
                wav.data_size);

    rkcam::AacEncoderConfig enc_cfg;
    enc_cfg.stream_id = "audio0";

    enc_cfg.input_sample_rate = wav.sample_rate;
    enc_cfg.input_channels = wav.channels;
    enc_cfg.input_format = rkcam::AudioSampleFormat::S16LE;

    enc_cfg.output_sample_rate = wav.sample_rate;
    enc_cfg.output_channels = 1;
    enc_cfg.bit_rate = bit_rate;

    // /*
    //  * 你的板子当前 2ch 采集，但只有左声道有效。
    //  */
    if (wav.channels >= 2) {
        enc_cfg.channel_select = rkcam::AudioChannelSelect::LeftToMono;
        enc_cfg.output_channels = 1;
    } else {
        enc_cfg.channel_select = rkcam::AudioChannelSelect::Keep;
        enc_cfg.output_channels = wav.channels;
    }

    rkcam::AacEncoder encoder(enc_cfg);
    if (!encoder.init()) {
        std::fprintf(stderr, "AacEncoder init failed\n");
        return 1;
    }

    std::printf("aac encoder: frame_size=%d output_rate=%d output_channels=%d extradata=%zu\n",
                encoder.frameSize(),
                encoder.outputSampleRate(),
                encoder.outputChannels(),
                encoder.extradata().size());

    std::FILE* in = std::fopen(input_wav.c_str(), "rb");
    if (!in) {
        std::fprintf(stderr, "open input wav failed: %s\n",
                     std::strerror(errno));
        return 1;
    }

    if (std::fseek(in, wav.data_offset, SEEK_SET) != 0) {
        std::fprintf(stderr, "seek wav data failed\n");
        std::fclose(in);
        return 1;
    }

    std::FILE* out = std::fopen(output_aac.c_str(), "wb");
    if (!out) {
        std::fprintf(stderr, "open output aac failed: %s\n",
                     std::strerror(errno));
        std::fclose(in);
        return 1;
    }

    const int frame_samples = encoder.frameSize();
    const int input_frame_bytes = wav.block_align;
    const size_t pcm_chunk_bytes =
        static_cast<size_t>(frame_samples) *
        static_cast<size_t>(input_frame_bytes);

    std::vector<uint8_t> pcm(pcm_chunk_bytes);

    uint32_t remaining = wav.data_size;
    int64_t input_sample_pos = 0;

    int input_frames = 0;
    int output_packets = 0;
    uint64_t output_bytes = 0;

    while(remaining > 0)
    {
        const size_t to_read = std::min<size_t>(pcm_chunk_bytes, remaining);
        std::memset(pcm.data(), 0, pcm.size());

        const size_t read_bytes = std::fread(pcm.data(), 1, to_read, in);
        if (read_bytes == 0) {
            break;
        }

        remaining -= static_cast<uint32_t>(read_bytes);

        int read_samples = static_cast<int>(read_bytes / input_frame_bytes);
        if (read_samples <= 0) {
            break;
        }
        /*
         * AacEncoder 第一版要求每次输入 frame_size。
         * 最后一帧不足时，已经 memset 成 0，相当于补静音。
         */
        if (read_samples < frame_samples) {
            std::printf("pad last pcm frame: read_samples=%d frame_samples=%d\n",
                        read_samples,
                        frame_samples);
            read_samples = frame_samples;
        }

        auto audio_buffer = std::make_shared<rkcam::AudioBuffer>();
        audio_buffer->data = pcm;

        rkcam::PipelineAudioFrame frame;
        frame.stream_id = "audio0";
        frame.sample_rate = wav.sample_rate;
        frame.channels = wav.channels;
        frame.format = rkcam::AudioSampleFormat::S16LE;
        frame.nb_samples = frame_samples;
        frame.pts_us = audioSamplesToUs(input_sample_pos, wav.sample_rate);
        frame.duration_us = audioSamplesToUs(frame_samples, wav.sample_rate);
        frame.frame_id = input_frames;
        frame.buffer = std::move(audio_buffer);

        std::vector<rkcam::EncodedPacket> packets;
        if (!encoder.encode(frame, packets)) {
            std::fprintf(stderr, "encoder.encode failed at input_frame=%d\n",
                         input_frames);
            std::fclose(in);
            std::fclose(out);
            return 1;
        }

        for(const auto& pkt : packets)
        {
            if(!writeAacAdtsPacket(out, pkt, encoder.outputSampleRate(), encoder.outputChannels()))
            {
                std::fprintf(stderr, "write ADTS packet failed\n");
                std::fclose(in);
                std::fclose(out);
                return 1;
            }

            ++output_packets;
            output_bytes += pkt.size() + 7;
        }

        input_sample_pos += frame_samples;
        ++input_frames;

        if (input_frames % 50 == 0) {
            std::printf("encoded input_frames=%d output_packets=%d pts=%.3f sec\n",
                        input_frames,
                        output_packets,
                        input_sample_pos * 1.0 / wav.sample_rate);
        }

    }

    std::vector<rkcam::EncodedPacket> flush_packets;
    if (!encoder.flush(flush_packets)) {
        std::fprintf(stderr, "encoder.flush failed\n");
        std::fclose(in);
        std::fclose(out);
        return 1;
    }

    for (const auto& pkt : flush_packets) {
        if (!writeAacAdtsPacket(
                out,
                pkt,
                encoder.outputSampleRate(),
                encoder.outputChannels())) {
            std::fprintf(stderr, "write flush ADTS packet failed\n");
            std::fclose(in);
            std::fclose(out);
            return 1;
        }

        ++output_packets;
        output_bytes += pkt.size() + 7;
    }

    std::fclose(in);
    std::fclose(out);

    encoder.close();

    std::printf("done\n");
    std::printf("input_frames=%d output_packets=%d output_bytes=%llu output=%s\n",
                input_frames,
                output_packets,
                static_cast<unsigned long long>(output_bytes),
                output_aac.c_str());

    return 0;

}