#pragma once

#include "rkcam/video/video_source.hpp"

#include <linux/videodev2.h>

#include <string>
#include <vector>

namespace rkcam{

struct V4L2VideoSourceConfig{
    std::string device = "/dev/video0";
    int width = 1920;
    int height = 1080;
    std::string pixel_format = "NV12";
    int buffer_count = 4;
};

class V4L2VideoSource : public IVideoSource{
public:
    explicit V4L2VideoSource(const V4L2VideoSourceConfig& config);
    ~V4L2VideoSource() override;

    bool open() override;
    bool start() override;
    bool readFrame(VideoFrame& frame) override;
    bool releaseFrame(const VideoFrame& frame) override;
    bool stop() override;
    bool close() override;
    bool updateConfig(const V4L2VideoSourceConfig& config);

private:
    struct MappedPlane{
        void* start = nullptr;
        size_t length = 0;
    };
    struct MappedBuffer{
        std::vector<MappedPlane> planes;
    };
    enum class WaitResult {
        Ready,
        Timeout,
        Error,
    };
private:
    bool queryCapability();
    void enumFrameSizes(uint32_t pixfmt);
    bool enumFormats();
    bool setFormat();
    bool initMmap();
    bool queueBuffer(uint32_t index);
    bool queueAllBuffer();
    WaitResult waitForFrame(int timeout_sec);
    void cleanupBuffers();

    bool configure();

    static int xioctl(int fd, unsigned long request, void* arg);

    static PixelFormat stringToPixelFormat(const std::string& fmt);//字符串 → 项目内部 PixelFormat

    static uint32_t pixelFormatToV4L2(PixelFormat fmt);//项目格式转 V4L2 fourcc
    static PixelFormat v4l2ToPixelFormat(uint32_t v4l2_fmt);//V4L2 fourcc 转项目格式

    std::string fourccToString(uint32_t fmt);

private:
    V4L2VideoSourceConfig config_;

    int fd_ = -1;
    bool opened_ = false;
    bool streaming_ = false;   //是否已经 VIDIOC_STREAMON
    bool buffers_initialized_ = false; //是否已经 VIDIOC_REQBUFS + mmap
    bool configured_ = false; //是否已经 VIDIOC_S_FMT 成功
    bool is_mplane_ =false;

    uint32_t v4l2_pixfmt_ = 0;
    PixelFormat pixel_format_ = PixelFormat::Unknown;

    v4l2_buf_type type_ = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    uint32_t num_planes_ = 1;

    int width_ = 0;
    int height_ = 0;
    int stride_ = 0;
    int64_t frame_id_ = 0;
    
    std::vector<MappedBuffer> buffers_;
    std::vector<bool> buffer_in_user_;

};


}

