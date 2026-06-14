/**
*@file v4l2_capture_test.cpp
*@brief 
*@author HKH368
*@date 2026-05-29
*@return 
*/

#include "rkcam/core/log.hpp"
#include "rkcam/video/v4l2_video_source.hpp"

#include <linux/videodev2.h>	//v4l2核心头文件

//分类标签：<sys/...> —— “硬件与内核直连通道”
#include <sys/ioctl.h>//用于ioctl
#include <sys/mman.h>
//分类标签：普通 .h —— “通用操作系统服务”
#include <fcntl.h> //用于open
#include <unistd.h>
#include <errno.h>

//分类标签：无 .h 后缀（以及 c 开头的头文件） —— “现代 C++ 语言工具箱”
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cerrno>
#include <chrono>



static std::string fourccToString(uint32_t fmt)
{
    char s[5];
    s[0] = static_cast<char>(fmt & 0xff);
    s[1] = static_cast<char>((fmt >> 8) & 0xff);
    s[2] = static_cast<char>((fmt >> 16) & 0xff);
    s[3] = static_cast<char>((fmt >> 24) & 0xff);
    s[4] = '\0';
    return std::string(s);
}


struct PlaneBuffer{
    void* start = nullptr;
    size_t length = 0;
};
struct Buffer{
    std::vector<PlaneBuffer> planes;
};
struct V4L2Context{
    int fd = -1;
    std::vector<Buffer> buffers;
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    bool is_mplane=false;
    uint32_t num_planes = 1;
};
struct Options{
	std::string device = "/dev/video0";
	int width = 800;
	int height = 600;
	std::string format = "NV12";
	int count = 100;
	std::string output_dir = "/userdata/rkcam/output/test.yuv";
	bool info_only = false;
};
static void printUsage(const char *prog){
    std::printf(
        "Usage: \n"
        "%s --device /dev/video0 --width 800 --height 600 --format YUYV --count 100 --output_dir /userdata/rkcam/output/test.yuyv\n"
        "Options:\n"
        "  --device <de>   Video device (default: /dev/video0)\n"
        "  --width  <w>      Frame width (default:800)\n"
        "  --height <h>      Frame height (default:600)\n"
        "  --format <fmt>      Pixel format (default:YUYV)\n"
        "  --count  <n>      Number of frames to capture (default:100)\n"
        "  --output <file>    default: /userdata/rkcam/output/test.yuv\n"
        "  --info             query capability and formats only\n",
        prog
    );
}
static bool parseArgs(int argc, char **argv, Options &opt){
	
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        auto needValue = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                RKCAM_LOGE("Missing value for %s", name);
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--device") {
            const char* v = needValue("--device");
            if (!v) return false;
            opt.device = v;
        } else if (arg == "--width") {
            const char* v = needValue("--width");
            if (!v) return false;
            opt.width = std::atoi(v);
        } else if (arg == "--height") {
            const char* v = needValue("--height");
            if (!v) return false;
            opt.height = std::atoi(v);
        } else if (arg == "--format") {
            const char* v = needValue("--format");
            if (!v) return false;
            opt.format = v;
        } else if (arg == "--count") {
            const char* v = needValue("--count");
            if (!v) return false;
            opt.count = std::atoi(v);
        } else if (arg == "--output") {
            const char* v = needValue("--output");
            if (!v) return false;
            opt.output_dir = v;
        } else if (arg == "--info") {
            opt.info_only = true;
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return false;
        } else {
            RKCAM_LOGE("Unknown argument: %s", arg.c_str());
            printUsage(argv[0]);
            return false;
        }
    }
    return true;
}

static int xioctl(int fd, unsigned long request, void *arg){
    int ret;
    do {
        ret = ioctl(fd, request, arg);
    }while(ret == -1 && errno == EINTR);

    return ret;
}

static void cleanup(V4L2Context &ctx){
    for(auto& buffer : ctx.buffers){
        for(auto& plane:buffer.planes){
            if(plane.start && plane.start != MAP_FAILED){
                munmap(plane.start, plane.length);
                plane.start = nullptr;
                plane.length = 0;
            }
        }
    }
    ctx.buffers.clear();
    if(ctx.fd >= 0)
    {
        close(ctx.fd);
        ctx.fd = -1;
    }
}
bool openDevice(V4L2Context &ctx, const std::string &device){
    ctx.fd = open(device.c_str(), O_RDWR | O_NONBLOCK, 0);
    if(ctx.fd < 0){
        RKCAM_LOGE("failed to open device %s", device.c_str());
        return false;
    }
    return true;
    
}
bool queryCapability(V4L2Context &ctx){
    v4l2_capability cap;
    if(xioctl(ctx.fd, VIDIOC_QUERYCAP, &cap) < 0){
        RKCAM_LOGE("failed to query capability");
        return false;
    }
    RKCAM_LOGI("Driver:     %s", cap.driver);
    RKCAM_LOGI("Card:       %s", cap.card);
    RKCAM_LOGI("Bus info:   %s", cap.bus_info);
    RKCAM_LOGI("Version:    %u.%u.%u", (cap.version >> 16) & 0xff, (cap.version >> 8) & 0xff, cap.version & 0xff);

    uint32_t caps = cap.capabilities;
    if(caps & V4L2_CAP_DEVICE_CAPS){
        caps = cap.device_caps;
    }
    bool has_capture = caps &V4L2_CAP_VIDEO_CAPTURE;
    bool has_capture_mplane = caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE;
    if (!has_capture && !has_capture_mplane){
        RKCAM_LOGE("device does not support video capture (neither single nor multi-planar)");
        return false;
    }

    if (has_capture_mplane) {
        ctx.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; // 多平面模式
        ctx.is_mplane = true;
        std::printf("[INFO] Device Mode: Multi-Planar (MPLANE)\n");
    } else {
        ctx.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;        // 传统单平面模式
        std::printf("[INFO] Device Mode: Single-Planar\n");
    }
    if(!(caps & V4L2_CAP_STREAMING)){
        ctx.is_mplane = false;
        RKCAM_LOGE("device does not support streaming io");
        return false;
    }
    return true;
    
}
static std::string toUpper(std::string str){
    auto upper = [](unsigned char c)->unsigned char{
        return static_cast<char>(std::toupper(c));
    };
    std::transform(str.begin(), str.end(), str.begin(), upper);
    return str;
}
static uint32_t pixelFormatFromString(const std::string &fmt){
    std::string f = toUpper(fmt);

    if(f == "YUYV")
    {
        return V4L2_PIX_FMT_YUYV;
    }
    if(f == "NV12")
    {
        return V4L2_PIX_FMT_NV12;
    }
    RKCAM_LOGE("Unsupported format string: %s", fmt.c_str());
    return 0;
}
static bool setFormat(V4L2Context &ctx, int width, int height, uint32_t pixfmt){
    v4l2_format fmt{};
    fmt.type = ctx.type;
    if(ctx.is_mplane){
        fmt.fmt.pix_mp.width = width;
        fmt.fmt.pix_mp.height = height;
        fmt.fmt.pix_mp.pixelformat = pixfmt;
        fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
        if(xioctl(ctx.fd, VIDIOC_S_FMT, &fmt) < 0)
        {
            RKCAM_LOGE("VIDIOC_S_FMT MPLANE failed: %s", strerror(errno));
            return false;
        }
        ctx.num_planes = fmt.fmt.pix_mp.num_planes;
        RKCAM_LOGI("Set MPLANE format:");
        RKCAM_LOGI("  width       : %u", fmt.fmt.pix_mp.width);
        RKCAM_LOGI("  height      : %u", fmt.fmt.pix_mp.height);
        RKCAM_LOGI("  pixelformat : %c%c%c%c",
                   fmt.fmt.pix_mp.pixelformat & 0xff,
                   (fmt.fmt.pix_mp.pixelformat >> 8) & 0xff,
                   (fmt.fmt.pix_mp.pixelformat >> 16) & 0xff,
                   (fmt.fmt.pix_mp.pixelformat >> 24) & 0xff);
        RKCAM_LOGI("  num_planes  : %u", ctx.num_planes);

        for (uint32_t i = 0; i < ctx.num_planes; ++i) {
            RKCAM_LOGI("  plane[%u]: bytesperline=%u sizeimage=%u",
                       i,
                       fmt.fmt.pix_mp.plane_fmt[i].bytesperline,
                       fmt.fmt.pix_mp.plane_fmt[i].sizeimage);
        }
    }
    else{
        fmt.fmt.pix.width = width;
        fmt.fmt.pix.height = height;
        fmt.fmt.pix.pixelformat = pixfmt;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
        ctx.num_planes = 1;
        if(xioctl(ctx.fd, VIDIOC_S_FMT, &fmt) < 0){
            RKCAM_LOGE("failed to set format");
            return false;
        }
        RKCAM_LOGI("Set single-plane format:");
        RKCAM_LOGI("  width       : %u", fmt.fmt.pix.width);
        RKCAM_LOGI("  height      : %u", fmt.fmt.pix.height);
        RKCAM_LOGI("  pixelformat : %c%c%c%c",
                   fmt.fmt.pix.pixelformat & 0xff,
                   (fmt.fmt.pix.pixelformat >> 8) & 0xff,
                   (fmt.fmt.pix.pixelformat >> 16) & 0xff,
                   (fmt.fmt.pix.pixelformat >> 24) & 0xff);
        RKCAM_LOGI("  bytesperline: %u", fmt.fmt.pix.bytesperline);
        RKCAM_LOGI("  sizeimage   : %u", fmt.fmt.pix.sizeimage);
        
    }
    return true;
}
static bool initMmap(V4L2Context &ctx, uint32_t buffer_count){
    v4l2_requestbuffers req{};
    req.count = buffer_count;
    req.type = ctx.type;
    req.memory = V4L2_MEMORY_MMAP;

    if(xioctl(ctx.fd, VIDIOC_REQBUFS, &req) < 0){
        RKCAM_LOGE("failed to request buffers");
        return false;   
    }

    if(req.count < 2){
        RKCAM_LOGE("Insufficient buffer memory on device");
        return false;
    }
    ctx.buffers.resize(req.count);
    for(uint32_t i = 0; i < req.count; i++){
        v4l2_buffer buf{};
        buf.type = ctx.type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        
        std::vector<v4l2_plane> planes;
        if(ctx.is_mplane){
            planes.resize(ctx.num_planes);
            buf.m.planes = planes.data();
            buf.length = ctx.num_planes;
        }

        if(xioctl(ctx.fd, VIDIOC_QUERYBUF, &buf) < 0){
            RKCAM_LOGE("failed to query buffer %u: %s", i, strerror(errno));
            return false;
        }
        if(ctx.is_mplane){
            ctx.buffers[i].planes.resize(ctx.num_planes);
            for(uint32_t p = 0; p < ctx.num_planes; p++)
            {
                ctx.buffers[i].planes[p].length = buf.m.planes[p].length;
                ctx.buffers[i].planes[p].start = mmap(
                    nullptr,
                    buf.m.planes[p].length,
                    PROT_READ | PROT_WRITE,
                    MAP_SHARED,
                    ctx.fd,
                    buf.m.planes[p].m.mem_offset
                );
                if(ctx.buffers[i].planes[p].start == MAP_FAILED){
                    RKCAM_LOGE("mmap buffer=%u plane=%u failed: %s",
                               i, p, strerror(errno));
                    ctx.buffers[i].planes[p].start = nullptr;
                    return false;
                }
                RKCAM_LOGI("mmap buffer[%u] plane[%u], length=%zu",
                           i, p, ctx.buffers[i].planes[p].length);
            }

        }
        else{
            ctx.buffers[i].planes[0].length = buf.length;
            ctx.buffers[i].planes[0].start = mmap(
                nullptr,
                buf.length,
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                ctx.fd, //只有输入摄像头的fd，才会调用对应摄像头的底层mmap驱动函数。注意与匿名映射区别
                buf.m.offset
            );
            if(ctx.buffers[i].planes[0].start == MAP_FAILED){
                RKCAM_LOGE("failed to mmap buffer %u: %s", i, strerror(errno));
                ctx.buffers[i].planes[0].start = nullptr;
                return false;
            }
            RKCAM_LOGI("mmap buffer[%u], length=%zu",
                       i, ctx.buffers[i].planes[0].length);
        }

    }
    return true;
}

#define VIDEO_MAX_PLANES 8
static bool queueBuffer(V4L2Context &ctx, uint32_t index){
    v4l2_buffer buf{};
    v4l2_plane planes[VIDEO_MAX_PLANES] {};

    buf.type = ctx.type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;

    if(ctx.is_mplane){
        buf.length = ctx.num_planes;
        buf.m.planes = planes;
    }
    if(xioctl(ctx.fd, VIDIOC_QBUF, &buf) < 0){
        RKCAM_LOGE("VIDIOC_QBUF index=%u fail: %s", index, strerror(errno));
        return false;
    }
    return true;
}
static bool queueAllBuffers(V4L2Context &ctx){
    for(uint32_t i = 0; i < ctx.buffers.size(); i++){
        if(!(queueBuffer(ctx, i)))
        {
            return false;
        }
    }
    return true;
}
static bool startCapturing(V4L2Context& ctx)
{
    if(!queueAllBuffers(ctx)){
        return false;
    }
    v4l2_buf_type type = ctx.type;
    if(xioctl(ctx.fd, VIDIOC_STREAMON, &type)){
        RKCAM_LOGI("VIDIOC_STREAMON failed: %s", strerror(errno));
        return false;
    }
    RKCAM_LOGI("stream on");
    return true;
}
static bool stopCapturing(V4L2Context& ctx){
    v4l2_buf_type type = ctx.type;
    if(xioctl(ctx.fd, VIDIOC_STREAMOFF, &type) < 0){
        RKCAM_LOGE("VIDIOC_STREAMOFF failed: %s", strerror(errno));
    } else {
        RKCAM_LOGI("stream off");
    }
    return true;
}

static bool captureFrames(V4L2Context& ctx, int frame_count, const std::string& output){
    FILE* fp = fopen(output.c_str(), "wb");
    if(!fp){
        RKCAM_LOGE("open output failed: %s", output.c_str());
        return false;
    }
    int captured = 0;
    while(captured < frame_count){
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(ctx.fd, &fds);

        timeval tv {};
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        int ret = select(ctx.fd + 1, &fds, nullptr, nullptr, &tv);
        if(ret < 0){
            if(errno == EINTR){
                continue;
            }
            RKCAM_LOGE("select failed: %s", strerror(errno));
            fclose(fp);
            return false;
        }
        if (ret == 0) {
            RKCAM_LOGE("select timeout");
            continue;
        }
        v4l2_buffer buf{};
        v4l2_plane planes[VIDEO_MAX_PLANES] {};
        buf.type = ctx.type;
        buf.memory = V4L2_MEMORY_MMAP;

        if(ctx.is_mplane){
            buf.length = ctx.num_planes;
            buf.m.planes = planes;
        }

        if(xioctl(ctx.fd, VIDIOC_DQBUF, &buf)){
            if (errno == EAGAIN) {
                continue;
            }
            RKCAM_LOGE("VIDIOC_DQBUF failed: %s", strerror(errno));
            fclose(fp);
            return false;
        }
        if(buf.index >= ctx.buffers.size())
        {
            RKCAM_LOGE("invalid buffer index: %u", buf.index);
            fclose(fp);
            return false;
        }
        if(ctx.is_mplane){
            for(uint32_t p = 0; p < ctx.num_planes; ++p){
                size_t bytes = buf.m.planes[p].bytesused;
                fwrite(ctx.buffers[buf.index].planes[p].start, 1, bytes, fp);
                RKCAM_LOGI("frame=%d buffer=%u plane=%u bytesused=%zu",
                captured, buf.index, p, bytes);
            }

        }
        else{
            size_t bytes = buf.bytesused;
            fwrite(ctx.buffers[buf.index].planes[0].start, 1, bytes, fp);
            RKCAM_LOGI("frame=%d buffer=%u bytesused=%zu",
            captured, buf.index, bytes);
        }
        if(!queueBuffer(ctx, buf.index)){
            fclose(fp);
            return false;
        }
        ++captured;

    }
    fclose(fp);
    RKCAM_LOGI("capture done, frames=%d", captured);
    return true;
}
static void enumFrameSizes(V4L2Context& ctx, uint32_t pixfmt){
    for(uint32_t i = 0; ; ++i){
        v4l2_frmsizeenum size {};
        size.index = i;
        size.pixel_format = pixfmt;

        if(xioctl(ctx.fd, VIDIOC_ENUM_FRAMESIZES, &size) < 0){
            break;
        }
        if(size.type == V4L2_FRMSIZE_TYPE_DISCRETE)
        {
            RKCAM_LOGI("    size[%u]: %ux%u",
                    i,
                    size.discrete.width,
                    size.discrete.height);
        }
        else if(size.type == V4L2_FRMSIZE_TYPE_STEPWISE){
            RKCAM_LOGI("    size[%u]: stepwise min=%ux%u max=%ux%u step=%ux%u",
            i,
            size.stepwise.min_width,
            size.stepwise.min_height,
            size.stepwise.max_width,
            size.stepwise.max_height,
            size.stepwise.step_width,
            size.stepwise.step_height);
        }
        else if (size.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
            RKCAM_LOGI("    size[%u]: continuous min=%ux%u max=%ux%u",
                       i,
                       size.stepwise.min_width,
                       size.stepwise.min_height,
                       size.stepwise.max_width,
                       size.stepwise.max_height);
        }

    }
}
static bool enumFormats(V4L2Context& ctx)
{
    RKCAM_LOGI("Enumerating formats:");

    for (uint32_t i = 0; ; ++i) {
        v4l2_fmtdesc desc {};
        desc.index = i;
        desc.type = ctx.type;

        if (xioctl(ctx.fd, VIDIOC_ENUM_FMT, &desc) < 0) {
            if (errno == EINVAL) {
                break;
            }
            RKCAM_LOGE("VIDIOC_ENUM_FMT failed: %s", std::strerror(errno));
            return false;
        }

        RKCAM_LOGI("  fmt[%u]: %s, description=%s",
                   i,
                   fourccToString(desc.pixelformat).c_str(),
                   desc.description);

        enumFrameSizes(ctx, desc.pixelformat);
    }

    return true;
}

// int main(int argc, char **argv)
// {
//     Options opt;
//     V4L2Context ctx;
//     if(!parseArgs(argc, argv, opt)){
//         return -1;
//     }

//     if(!openDevice(ctx, opt.device)){
//         cleanup(ctx);
//         return -1;
//     }

//     if(!queryCapability(ctx)){
//         cleanup(ctx);
//         return -1;
//     }

//     if (!enumFormats(ctx)) {
//         cleanup(ctx);
//         return 1;
//     }

//     if (opt.info_only) {
//         cleanup(ctx);
//         RKCAM_LOGI("info query done");
//         return 0;
//     }

//     uint32_t pixfmt = pixelFormatFromString(opt.format);
//     if(pixfmt == 0){
//         cleanup(ctx);
//         return -1;
//     }
//     if(!setFormat(ctx, opt.width, opt.height, pixfmt)){
//         cleanup(ctx);
//         return -1;
//     }
//     if(!initMmap(ctx, 4)){
//         cleanup(ctx);
//         return -1;
//     }
//     if(!(startCapturing(ctx))){
//         cleanup(ctx);
//         return -1;
//     }

//     bool ok = captureFrames(ctx, opt.count, opt.output_dir);

//     stopCapturing(ctx);
//     cleanup(ctx);
//     return ok ? 0:-1;




//     return 0;
// }

int main(int argc, char **argv)
{
    Options opt;
    V4L2Context ctx;
    if(!parseArgs(argc, argv, opt)){
        return -1;
    }
    rkcam::V4L2VideoSourceConfig config;
    config.device = opt.device;
    config.width = opt.width;
    config.height = opt.height;
    config.pixel_format = opt.format;
    config.buffer_count = 4;
    rkcam::V4L2VideoSource source(config);

    if(!source.open())
    {
        RKCAM_LOGE("source.open failed");
        return 1;
    }
    if(!source.start())
    {
        RKCAM_LOGE("source.start failed");
        source.close();
        return 1;
    }
    FILE* fp = fopen(opt.output_dir.c_str(), "wb");
    if(!fp)
    {
        RKCAM_LOGE("open output failed: %s", opt.output_dir.c_str());
        source.stop();
        source.close();
        return 1;
    }
    int captured = 0;
    size_t total_bytes = 0;
    auto t0 = std::chrono::steady_clock::now();

    while(captured < opt.count)
    {
        rkcam::VideoSourceFrame frame;
        if(!source.readFrame(frame))
        {
            RKCAM_LOGE("readFrame failed");
            continue;
        }
        for(const auto& plane : frame.planes)
        {
            if(plane.data && plane.bytesused > 0)
            {
                std::fwrite(plane.data, 1, plane.bytesused, fp);
                total_bytes += plane.bytesused;
            }
        }
        if(!source.releaseFrame(frame))
        {
            RKCAM_LOGE("releaseFrame failed");
            break;
        }
        captured++;
        if (captured % 30 == 0 || captured == opt.count) {
            RKCAM_LOGI("captured %d/%d", captured, opt.count);
            // RKCAM_LOGI("captured palne[0] stride: &d", frame.planes[0].stride);
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();
    double fps = elapsed > 0.0 ? static_cast<double>(captured) / elapsed : 0.0;

    std::fclose(fp);

    source.stop();
    source.close();

    RKCAM_LOGI("capture done");
    RKCAM_LOGI("frames     : %d", captured);
    RKCAM_LOGI("total bytes: %zu", total_bytes);
    RKCAM_LOGI("elapsed    : %.3f sec", elapsed);
    RKCAM_LOGI("avg fps    : %.2f", fps);

    return captured == opt.count ? 0 : 1;

}
