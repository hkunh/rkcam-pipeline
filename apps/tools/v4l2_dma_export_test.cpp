#include "rkcam/core/log.hpp"
#include "rkcam/video/v4l2_video_source.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

static bool isValidFd(int fd)
{
    if (fd < 0) {
        return false;
    }

    return ::fcntl(fd, F_GETFD) != -1;
}

struct Options {
    std::string device = "/dev/video0";
    int width = 1920;
    int height = 1080;
    std::string format = "NV12";
    int count = 10;
};

static bool parseArgs(int argc, char** argv, Options& opt)
{
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        auto needValue = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                RKCAM_LOGE("missing value for %s", name);
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
        } else {
            RKCAM_LOGE("unknown argument: %s", arg.c_str());
            return false;
        }
    }

    return true;
}

int main(int argc, char** argv)
{
    Options opt;
    if (!parseArgs(argc, argv, opt)) {
        return 1;
    }

    rkcam::V4L2VideoSourceConfig config;
    config.device = opt.device;
    config.width = opt.width;
    config.height = opt.height;
    config.pixel_format = opt.format;
    config.buffer_count = 4;
    config.export_dma_fd = true;

    rkcam::V4L2VideoSource source(config);

    if (!source.open()) {
        RKCAM_LOGE("source.open failed");
        return 1;
    }

    if (!source.start()) {
        RKCAM_LOGE("source.start failed");
        source.close();
        return 1;
    }

    RKCAM_LOGI("dma export test start: device=%s width=%d height=%d format=%s count=%d",
               opt.device.c_str(),
               opt.width,
               opt.height,
               opt.format.c_str(),
               opt.count);

    int captured = 0;

    while (captured < opt.count) {
        rkcam::VideoSourceFrame frame;

        if (!source.readFrame(frame)) {
            continue;
        }

        RKCAM_LOGI("frame_id=%lld pts_us=%lld buffer_index=%d width=%d height=%d planes=%zu",
                   static_cast<long long>(frame.frame_id),
                   static_cast<long long>(frame.pts_us),
                   frame.buffer_index,
                   frame.width,
                   frame.height,
                   frame.planes.size());

        for (size_t p = 0; p < frame.planes.size(); ++p) {
            const auto& plane = frame.planes[p];

            RKCAM_LOGI("  plane[%zu]: dma_fd=%d valid=%d stride=%d length=%zu bytesused=%zu data=%p",
                       p,
                       plane.dma_fd,
                       isValidFd(plane.dma_fd) ? 1 : 0,
                       plane.stride,
                       plane.length,
                       plane.bytesused,
                       plane.data);
        }

        if (!source.releaseFrame(frame)) {
            RKCAM_LOGE("releaseFrame failed");
            break;
        }

        ++captured;
    }

    source.stop();
    source.close();

    RKCAM_LOGI("dma export test done, captured=%d", captured);

    return captured == opt.count ? 0 : 1;
}