#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

struct NalUnit {
    const uint8_t* start_code = nullptr;
    size_t start_code_len = 0;

    const uint8_t* data = nullptr;
    size_t size = 0;

    const uint8_t* end = nullptr;
};

static std::string ffmpegErrorToString(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] {};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

static bool readFile(const std::string& path, std::vector<uint8_t>& data)
{
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        std::fprintf(stderr, "open input failed: %s\n", path.c_str());
        return false;
    }

    std::fseek(fp, 0, SEEK_END);
    const long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);

    if (size <= 0) {
        std::fclose(fp);
        std::fprintf(stderr, "input file is empty: %s\n", path.c_str());
        return false;
    }

    data.resize(static_cast<size_t>(size));

    const size_t n = std::fread(data.data(), 1, data.size(), fp);
    std::fclose(fp);

    if (n != data.size()) {
        std::fprintf(stderr, "read input failed: expected=%zu got=%zu\n",
                     data.size(), n);
        return false;
    }

    return true;
}

static const uint8_t* findStartCode(
    const uint8_t* begin,
    const uint8_t* end,
    size_t& start_code_len)
{
    start_code_len = 0;

    if (!begin || !end || begin >= end) {
        return nullptr;
    }

    for (const uint8_t* p = begin; p + 3 <= end; ++p) {
        if (p[0] == 0x00 &&
            p[1] == 0x00 &&
            p[2] == 0x01) {
            start_code_len = 3;
            return p;
        }

        if (p + 4 <= end &&
            p[0] == 0x00 &&
            p[1] == 0x00 &&
            p[2] == 0x00 &&
            p[3] == 0x01) {
            start_code_len = 4;
            return p;
        }
    }

    return nullptr;
}

static bool splitAnnexBNals(
    const uint8_t* data,
    size_t size,
    std::vector<NalUnit>& nals)
{
    nals.clear();

    if (!data || size == 0) {
        return false;
    }

    const uint8_t* end = data + size;

    size_t sc_len = 0;
    const uint8_t* sc = findStartCode(data, end, sc_len);

    if (!sc) {
        return false;
    }

    while (sc) {
        const uint8_t* nal_start = sc + sc_len;

        size_t next_sc_len = 0;
        const uint8_t* next_sc = findStartCode(nal_start, end, next_sc_len);

        const uint8_t* nal_end = next_sc ? next_sc : end;

        while (nal_end > nal_start && *(nal_end - 1) == 0x00) {
            --nal_end;
        }

        if (nal_end > nal_start) {
            NalUnit nal;
            nal.start_code = sc;
            nal.start_code_len = sc_len;
            nal.data = nal_start;
            nal.size = static_cast<size_t>(nal_end - nal_start);
            nal.end = nal_end;
            nals.push_back(nal);
        }

        sc = next_sc;
        sc_len = next_sc_len;
    }

    return !nals.empty();
}

static uint8_t h264NalType(const NalUnit& nal)
{
    if (!nal.data || nal.size == 0) {
        return 0;
    }

    return nal.data[0] & 0x1f;
}

static bool extractSpsPps(
    const std::vector<NalUnit>& nals,
    std::vector<uint8_t>& sps,
    std::vector<uint8_t>& pps)
{
    sps.clear();
    pps.clear();

    for (const auto& nal : nals) {
        const uint8_t type = h264NalType(nal);

        if (type == 7 && sps.empty()) {
            sps.assign(nal.data, nal.data + nal.size);
        } else if (type == 8 && pps.empty()) {
            pps.assign(nal.data, nal.data + nal.size);
        }
    }

    return !sps.empty() && !pps.empty();
}

static void appendStartCode4(std::vector<uint8_t>& out)
{
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x01);
}

static bool buildAnnexBExtradata(
    const std::vector<uint8_t>& sps,
    const std::vector<uint8_t>& pps,
    std::vector<uint8_t>& extradata)
{
    extradata.clear();

    if (sps.empty() || pps.empty()) {
        return false;
    }

    appendStartCode4(extradata);
    extradata.insert(extradata.end(), sps.begin(), sps.end());

    appendStartCode4(extradata);
    extradata.insert(extradata.end(), pps.begin(), pps.end());

    return true;
}

static bool isVideoNal(uint8_t type)
{
    return type == 1 || type == 5;
}

static bool setupStreamCodecparOnly(
    AVStream* st,
    int width,
    int height,
    const std::vector<uint8_t>& extradata)
{
    AVCodecParameters* par = st->codecpar;
    if (!par) {
        return false;
    }

    par->codec_type = AVMEDIA_TYPE_VIDEO;
    par->codec_id = AV_CODEC_ID_H264;
    par->codec_tag = 0;
    par->width = width;
    par->height = height;
    par->format = AV_PIX_FMT_YUV420P;

    par->extradata = static_cast<uint8_t*>(
        av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));

    if (!par->extradata) {
        return false;
    }

    std::memcpy(par->extradata, extradata.data(), extradata.size());
    par->extradata_size = static_cast<int>(extradata.size());

    return true;
}

static bool setupStreamLegacyOnly(
    AVStream* st,
    int width,
    int height,
    const std::vector<uint8_t>& extradata)
{
    AVCodecParameters* par = st->codecpar;
    if (par) {
        par->codec_type = AVMEDIA_TYPE_VIDEO;
        par->codec_id = AV_CODEC_ID_H264;
        par->codec_tag = 0;
        par->width = width;
        par->height = height;
        par->format = AV_PIX_FMT_YUV420P;
        par->extradata = nullptr;
        par->extradata_size = 0;
    }

#if FF_API_LAVF_AVCTX
    if (!st->codec) {
        std::fprintf(stderr, "legacy mode failed: st->codec is null\n");
        return false;
    }

    AVCodecContext* ctx = st->codec;

    ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    ctx->codec_id = AV_CODEC_ID_H264;
    ctx->codec_tag = 0;
    ctx->width = width;
    ctx->height = height;
    ctx->time_base = st->time_base;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    ctx->extradata = static_cast<uint8_t*>(
        av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));

    if (!ctx->extradata) {
        return false;
    }

    std::memcpy(ctx->extradata, extradata.data(), extradata.size());
    ctx->extradata_size = static_cast<int>(extradata.size());

    return true;
#else
    std::fprintf(stderr, "legacy mode not supported by this FFmpeg header\n");
    return false;
#endif
}

static bool setupStreamBothManual(
    AVStream* st,
    int width,
    int height,
    const std::vector<uint8_t>& extradata)
{
    if (!setupStreamCodecparOnly(st, width, height, extradata)) {
        return false;
    }

#if FF_API_LAVF_AVCTX
    if (!st->codec) {
        std::fprintf(stderr, "both mode failed: st->codec is null\n");
        return false;
    }

    AVCodecContext* ctx = st->codec;

    ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    ctx->codec_id = AV_CODEC_ID_H264;
    ctx->codec_tag = 0;
    ctx->width = width;
    ctx->height = height;
    ctx->time_base = st->time_base;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    ctx->extradata = static_cast<uint8_t*>(
        av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));

    if (!ctx->extradata) {
        return false;
    }

    std::memcpy(ctx->extradata, extradata.data(), extradata.size());
    ctx->extradata_size = static_cast<int>(extradata.size());

    return true;
#else
    return true;
#endif
}

static bool setupStreamLegacySync(
    AVStream* st,
    int width,
    int height,
    const std::vector<uint8_t>& extradata)
{
#if FF_API_LAVF_AVCTX
    if (!st->codec || !st->codecpar) {
        std::fprintf(stderr, "legacy_sync failed: codec=%p codecpar=%p\n",
                     st->codec,
                     st->codecpar);
        return false;
    }

    AVCodecContext* ctx = st->codec;

    ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    ctx->codec_id = AV_CODEC_ID_H264;
    ctx->codec_tag = 0;
    ctx->width = width;
    ctx->height = height;
    ctx->time_base = st->time_base;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    ctx->extradata = static_cast<uint8_t*>(
        av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));

    if (!ctx->extradata) {
        return false;
    }

    std::memcpy(ctx->extradata, extradata.data(), extradata.size());
    ctx->extradata_size = static_cast<int>(extradata.size());

    const int ret = avcodec_parameters_from_context(st->codecpar, ctx);
    if (ret < 0) {
        std::fprintf(stderr, "avcodec_parameters_from_context failed: %s\n",
                     ffmpegErrorToString(ret).c_str());
        return false;
    }

    st->codecpar->codec_tag = 0;

    return true;
#else
    return setupStreamCodecparOnly(st, width, height, extradata);
#endif
}

static void printStreamPointers(const char* tag, AVStream* st)
{
#if FF_API_LAVF_AVCTX
    std::fprintf(stderr,
                 "[%s] codec_extra=%p codec_extra_size=%d par_extra=%p par_extra_size=%d codec_id=%d par_id=%d\n",
                 tag,
                 st && st->codec ? st->codec->extradata : nullptr,
                 st && st->codec ? st->codec->extradata_size : 0,
                 st && st->codecpar ? st->codecpar->extradata : nullptr,
                 st && st->codecpar ? st->codecpar->extradata_size : 0,
                 st && st->codec ? st->codec->codec_id : -1,
                 st && st->codecpar ? st->codecpar->codec_id : -1);
#else
    std::fprintf(stderr,
                 "[%s] par_extra=%p par_extra_size=%d par_id=%d\n",
                 tag,
                 st && st->codecpar ? st->codecpar->extradata : nullptr,
                 st && st->codecpar ? st->codecpar->extradata_size : 0,
                 st && st->codecpar ? st->codecpar->codec_id : -1);
#endif
}

static bool writePackets(
    AVFormatContext* fmt,
    AVStream* st,
    const std::vector<NalUnit>& nals,
    int fps)
{
    const AVRational input_tb = AVRational{1, 1000000};

    int64_t frame_index = 0;
    const int64_t duration_us = 1000000LL / fps;

    for (const auto& nal : nals) {
        const uint8_t type = h264NalType(nal);

        if (!isVideoNal(type)) {
            continue;
        }

        AVPacket pkt;
        av_init_packet(&pkt);

        pkt.data = const_cast<uint8_t*>(nal.start_code);
        pkt.size = static_cast<int>(nal.end - nal.start_code);
        pkt.stream_index = st->index;
        pkt.pos = -1;

        const int64_t pts_us = frame_index * duration_us;

        pkt.pts = av_rescale_q(pts_us, input_tb, st->time_base);
        pkt.dts = pkt.pts;
        pkt.duration = av_rescale_q(duration_us, input_tb, st->time_base);

        if (type == 5) {
            pkt.flags |= AV_PKT_FLAG_KEY;
        }

        const int ret = av_write_frame(fmt, &pkt);
        if (ret < 0) {
            std::fprintf(stderr,
                         "av_write_frame failed: frame=%lld type=%u size=%d err=%s\n",
                         static_cast<long long>(frame_index),
                         type,
                         pkt.size,
                         ffmpegErrorToString(ret).c_str());
            return false;
        }

        ++frame_index;
    }

    std::fprintf(stderr, "written video packets: %lld\n",
                 static_cast<long long>(frame_index));

    return frame_index > 0;
}

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage:\n"
                     "  %s input.h264 output.mp4 mode [width height fps]\n\n"
                     "mode:\n"
                     "  codecpar\n"
                     "  legacy\n"
                     "  both\n"
                     "  legacy_sync\n\n"
                     "example:\n"
                     "  %s /userdata/rkcam/output/test.h264 /userdata/rkcam/output/test.mp4 legacy 640 360 30\n",
                     argv[0],
                     argv[0]);
        return 1;
    }

    const std::string input_path = argv[1];
    const std::string output_path = argv[2];
    const std::string mode = argv[3];

    const int width = argc > 4 ? std::atoi(argv[4]) : 640;
    const int height = argc > 5 ? std::atoi(argv[5]) : 360;
    const int fps = argc > 6 ? std::atoi(argv[6]) : 30;

    std::vector<uint8_t> h264;
    if (!readFile(input_path, h264)) {
        return 1;
    }

    std::vector<NalUnit> nals;
    if (!splitAnnexBNals(h264.data(), h264.size(), nals)) {
        std::fprintf(stderr, "split Annex-B NAL failed\n");
        return 1;
    }

    std::fprintf(stderr, "input=%s size=%zu nal_count=%zu\n",
                 input_path.c_str(),
                 h264.size(),
                 nals.size());

    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;

    if (!extractSpsPps(nals, sps, pps)) {
        std::fprintf(stderr, "extract SPS/PPS failed\n");
        return 1;
    }

    std::fprintf(stderr, "sps=%zu pps=%zu\n", sps.size(), pps.size());

    std::vector<uint8_t> extradata;
    if (!buildAnnexBExtradata(sps, pps, extradata)) {
        std::fprintf(stderr, "build extradata failed\n");
        return 1;
    }

    std::fprintf(stderr, "extradata=%zu mode=%s width=%d height=%d fps=%d\n",
                 extradata.size(),
                 mode.c_str(),
                 width,
                 height,
                 fps);

    AVFormatContext* fmt = nullptr;
    int ret = avformat_alloc_output_context2(
        &fmt,
        nullptr,
        "mp4",
        output_path.c_str());

    if (ret < 0 || !fmt) {
        std::fprintf(stderr, "avformat_alloc_output_context2 failed: %s\n",
                     ffmpegErrorToString(ret).c_str());
        return 1;
    }

    AVStream* st = avformat_new_stream(fmt, nullptr);
    if (!st) {
        std::fprintf(stderr, "avformat_new_stream failed\n");
        avformat_free_context(fmt);
        return 1;
    }

    st->time_base = AVRational{1, 1000000};
    st->avg_frame_rate = AVRational{fps, 1};
    st->r_frame_rate = AVRational{fps, 1};

    bool setup_ok = false;

    if (mode == "codecpar") {
        setup_ok = setupStreamCodecparOnly(st, width, height, extradata);
    } else if (mode == "legacy") {
        setup_ok = setupStreamLegacyOnly(st, width, height, extradata);
    } else if (mode == "both") {
        setup_ok = setupStreamBothManual(st, width, height, extradata);
    } else if (mode == "legacy_sync") {
        setup_ok = setupStreamLegacySync(st, width, height, extradata);
    } else {
        std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
        avformat_free_context(fmt);
        return 1;
    }

    if (!setup_ok) {
        std::fprintf(stderr, "setup stream failed, mode=%s\n", mode.c_str());
        avformat_free_context(fmt);
        return 1;
    }

    printStreamPointers("before avio_open", st);

    if (!(fmt->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&fmt->pb, output_path.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            std::fprintf(stderr, "avio_open failed: %s\n",
                         ffmpegErrorToString(ret).c_str());
            avformat_free_context(fmt);
            return 1;
        }
    }

    printStreamPointers("before write_header", st);

    ret = avformat_write_header(fmt, nullptr);
    if (ret < 0) {
        std::fprintf(stderr, "avformat_write_header failed: %s\n",
                     ffmpegErrorToString(ret).c_str());

        if (!(fmt->oformat->flags & AVFMT_NOFILE) && fmt->pb) {
            avio_closep(&fmt->pb);
        }

        avformat_free_context(fmt);
        return 1;
    }

    printStreamPointers("after write_header", st);

    if (!writePackets(fmt, st, nals, fps)) {
        std::fprintf(stderr, "writePackets failed\n");
    }

    std::fprintf(stderr, "av_write_trailer begin\n");
    ret = av_write_trailer(fmt);
    std::fprintf(stderr, "av_write_trailer end ret=%d\n", ret);

    if (!(fmt->oformat->flags & AVFMT_NOFILE) && fmt->pb) {
        std::fprintf(stderr, "avio_closep begin\n");
        avio_closep(&fmt->pb);
        std::fprintf(stderr, "avio_closep end\n");
    }

    printStreamPointers("before avformat_free_context", st);

    std::fprintf(stderr, "avformat_free_context begin\n");
    avformat_free_context(fmt);
    std::fprintf(stderr, "avformat_free_context end\n");

    std::fprintf(stderr, "done\n");
    return 0;
}