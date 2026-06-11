#include "rkcam/video/v4l2_video_source.hpp"
#include "rkcam/core/log.hpp"

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <cstring>
#include <cstdlib> //malloc(), free(), exit()
#include <chrono>

namespace rkcam{
V4L2VideoSource::V4L2VideoSource(const V4L2VideoSourceConfig& config):config_(config){

}
V4L2VideoSource::~V4L2VideoSource()
{
	close();
}

int V4L2VideoSource::xioctl(int fd, unsigned long request, void* arg){
	int ret;
	do{
		ret = ioctl(fd, request, arg);
	}while(ret < 0 && errno == EINTR);
	return ret;
}
std::string V4L2VideoSource::fourccToString(uint32_t fmt){
    char s[5];
    s[0] = static_cast<char>(fmt & 0xff);
    s[1] = static_cast<char>((fmt >> 8) & 0xff);
    s[2] = static_cast<char>((fmt >> 16) & 0xff);
    s[3] = static_cast<char>((fmt >> 24) & 0xff);
    s[4] = '\0';
    return std::string(s);
}
// static uint32_t V4L2VideoSource::stringToFourcc(const std::string& fmt){
// 	uint32_t ret;
// 	uint8_t a, b, c, d;
// 	a = fmt.c_str[0];
// 	b = fmt.c_str[1];
// 	c = fmt.c_str[2];
// 	d = fmt.c_Str[3];

// 	ret = (d << 24) & (c << 16) & (b << 8) & (a)
// 	return ret;
// }
PixelFormat V4L2VideoSource::stringToPixelFormat(const std::string& fmt){
	std::string f = fmt;
	for(auto& c : f){
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
	if (f == "NV12") return PixelFormat::NV12;
	return PixelFormat::Unknown;
}

uint32_t V4L2VideoSource::pixelFormatToV4L2(PixelFormat fmt){
	switch (fmt){
		case PixelFormat::NV12:
			return V4L2_PIX_FMT_NV12;
		default:
			return 0;
	}
}
PixelFormat V4L2VideoSource::v4l2ToPixelFormat(uint32_t v4l2_fmt){
	switch(v4l2_fmt){
		case V4L2_PIX_FMT_NV12:
			return PixelFormat::NV12;
		default:
			return PixelFormat::Unknown;
	}
}


bool V4L2VideoSource::open()
{
    if(opened_){
        return true;
    }

	fd_ = ::open(config_.device.c_str(), O_RDWR | O_NONBLOCK, 0);
	if(fd_ < 0){
		RKCAM_LOGE("open %s failed: %s", config_.device.c_str(), std::strerror(errno));
        return false;
	}
	RKCAM_LOGI("open video device: %s", config_.device.c_str());
	if (!queryCapability()) {
        close();
        return false;
    }
	opened_ = true;
    return true;

}
bool V4L2VideoSource::queryCapability(){
	v4l2_capability cap{};
	if(xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0)
	{
		RKCAM_LOGE("VIDIOC_QUERYCAP failed: %s", std::strerror(errno));
        return false;
	}
	RKCAM_LOGI("Driver	: %s", cap.driver);
	RKCAM_LOGI("Card	: %s", cap.card);
	RKCAM_LOGI("Bus_info	:%s, cap.bus_info");
	RKCAM_LOGI("Version 	:%u.%u.%u", 
	(cap.version>>16) & 0xff,
	(cap.version>>8) & 0xff,
	(cap.version) & 0xff);
	uint32_t caps = cap.capabilities;
	if(cap.capabilities & V4L2_CAP_DEVICE_CAPS){
		caps = cap.device_caps;
	}

	if(caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
	{
		type_ = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		is_mplane_ = true;
		 RKCAM_LOGI("Device mode: Video Capture Multiplanar");
	}
	else if(caps & V4L2_CAP_VIDEO_CAPTURE)
	{
		type_ = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		is_mplane_ = false;
		num_planes_ = 1;
		 RKCAM_LOGI("Device mode: Video Capture Single-Planar");
	}
	else {
        RKCAM_LOGE("Device does not support video capture");
        return false;
    }

	if(!(caps & V4L2_CAP_STREAMING))
	{
		RKCAM_LOGE("Device does not support streaming I/O");
        return false;
	}
	return true;
}


bool V4L2VideoSource::configure(){
	if (!opened_) {
        RKCAM_LOGE("device not opened");
        return false;
    }

	if (streaming_) {
        RKCAM_LOGE("cannot configure while streaming");
        return false;
    }
	if (buffers_initialized_) {
        cleanupBuffers();
        buffers_initialized_ = false;
    }


	pixel_format_  = stringToPixelFormat(config_.pixel_format);
	if (pixel_format_ == PixelFormat::Unknown) {
        RKCAM_LOGE("Unsupported pixel format string: %s", config_.pixel_format.c_str());
        return false;
    }

	v4l2_pixfmt_ = pixelFormatToV4L2(pixel_format_);
	if (v4l2_pixfmt_ == 0) {
        RKCAM_LOGE("Cannot convert PixelFormat to V4L2 fourcc");
        return false;
    }

	if(!setFormat()){
		return false;
	}
	configured_ = true;
	return true;

}

bool V4L2VideoSource::updateConfig(const V4L2VideoSourceConfig& config)
{
	if (!opened_) {
        RKCAM_LOGE("device not opened");
        return false;
    }

    if (streaming_) {
        RKCAM_LOGE("cannot update config while streaming, please stop first");
        return false;
    }

    if (buffers_initialized_) {
        cleanupBuffers();
        buffers_initialized_ = false;
    }

    config_ = config;
    configured_ = false;

    if (opened_) {
        return configure();
    }

    return true;
}

bool V4L2VideoSource::setFormat(){
	v4l2_format fmt {};
	fmt.type = type_;
	if(is_mplane_){
		fmt.fmt.pix_mp.width = static_cast<uint32_t>(config_.width);
		fmt.fmt.pix_mp.height = static_cast<uint32_t>(config_.height);
		fmt.fmt.pix_mp.pixelformat = v4l2_pixfmt_;
		fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
		if(xioctl(fd_,VIDIOC_S_FMT, &fmt) < 0)
		{
			RKCAM_LOGE("VIDIOC_S_FMT MPLANE failed: %s", std::strerror(errno));
            return false;
		}
		if (fmt.fmt.pix_mp.pixelformat != v4l2_pixfmt_) {

            RKCAM_LOGE("Driver changed pixel format: requested=%s actual=%s",
                       fourccToString(v4l2_pixfmt_).c_str(),
                       fourccToString(fmt.fmt.pix_mp.pixelformat).c_str());
            return false;
        }
		width_ = static_cast<int>(fmt.fmt.pix_mp.width);
        height_ = static_cast<int>(fmt.fmt.pix_mp.height);
        num_planes_ = fmt.fmt.pix_mp.num_planes;
		v4l2_pixfmt_ =fmt.fmt.pix_mp.pixelformat;
		pixel_format_ = v4l2ToPixelFormat(v4l2_pixfmt_);
        if (num_planes_ == 0 || num_planes_ > VIDEO_MAX_PLANES) {
            RKCAM_LOGE("Invalid num_planes: %u", num_planes_);
            return false;
        }

        RKCAM_LOGI("Set MPLANE format:");
        RKCAM_LOGI("  width       : %d", width_);
        RKCAM_LOGI("  height      : %d", height_);
        RKCAM_LOGI("  pixelformat : %s", fourccToString(v4l2_pixfmt_).c_str());
        RKCAM_LOGI("  num_planes  : %u", num_planes_);
		plane_strides_.clear();
		plane_strides_.resize(num_planes_);
		for(uint32_t i = 0; i < num_planes_; i++)
		{
			plane_strides_[i] =static_cast<int>(fmt.fmt.pix_mp.plane_fmt[i].bytesperline);
			RKCAM_LOGI("plane[%u]: bytesperline=%u sizeimage=%u",
						i,
						fmt.fmt.pix_mp.plane_fmt[i].bytesperline,
						fmt.fmt.pix_mp.plane_fmt[i].sizeimage);
		}
	}
	else{
		fmt.fmt.pix.width = static_cast<uint32_t>(config_.width);
		fmt.fmt.pix.height = static_cast<uint32_t>(config_.height);
		fmt.fmt.pix.pixelformat = v4l2_pixfmt_;
		fmt.fmt.pix.field = V4L2_FIELD_NONE;
		if(xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0)
		{
			RKCAM_LOGE("VIDIOC_S_FMT failed: %s", std::strerror(errno));
        	return false;
		}
		if(fmt.fmt.pix.pixelformat != v4l2_pixfmt_){

			RKCAM_LOGI("Driver changed pixel format: requested=%s actual=%s",
						fourccToString(v4l2_pixfmt_).c_str(),
						fourccToString(fmt.fmt.pix.pixelformat).c_str());
			return false;
		}
		width_ = static_cast<int>(fmt.fmt.pix.width);
    	height_ = static_cast<int>(fmt.fmt.pix.height);
		plane_strides_.clear();
		plane_strides_.push_back(static_cast<int>(fmt.fmt.pix.bytesperline));
		num_planes_ = 1;

		v4l2_pixfmt_ = fmt.fmt.pix.pixelformat;
    	pixel_format_ = v4l2ToPixelFormat(v4l2_pixfmt_);
		RKCAM_LOGI("Set single-plane format:");
		RKCAM_LOGI("  width       : %d", width_);
		RKCAM_LOGI("  height      : %d", height_);
		RKCAM_LOGI("  pixelformat : %s", fourccToString(v4l2_pixfmt_).c_str());
		RKCAM_LOGI("  bytesperline: %u", fmt.fmt.pix.bytesperline);
		RKCAM_LOGI("  sizeimage   : %u", fmt.fmt.pix.sizeimage);
		return true;
	}
	return true;
}




bool V4L2VideoSource::initMmap(){
	v4l2_requestbuffers req{};
	req.count = static_cast<uint32_t>(config_.buffer_count);
	req.type = type_;
	req.memory = V4L2_MEMORY_MMAP;
	if(xioctl(fd_, VIDIOC_REQBUFS, &req)){
		RKCAM_LOGE("VIDIOC_REQBUFS failed: %s", std::strerror(errno));
        return false;
	}
	if(req.count < 2){
		RKCAM_LOGE("Insufficient buffer count: %u", req.count);
        return false;
	}
	RKCAM_LOGI("Allocated V4L2 buffers: %u", req.count);
	buffers_.resize(req.count);
	buffer_in_user_.assign(req.count, false);
	for(uint32_t i = 0; i < req.count; i++){
		v4l2_buffer buf{};
		v4l2_plane planes[VIDEO_MAX_PLANES];
		buf.type = type_;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;


		if(is_mplane_){
			buf.length = num_planes_;
			buf.m.planes = planes;
		}

		if(xioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0)
		{
			RKCAM_LOGE("VIDIOC_QUERYBUF index=%u failed: %s",
                       i,
                       std::strerror(errno));
            return false;
		}

		if(is_mplane_){
			buffers_[i].planes.resize(num_planes_);
			for(uint32_t p = 0; p < num_planes_; p++){
				buffers_[i].planes[p].length = planes[p].length;
				buffers_[i].planes[p].start = mmap(
					nullptr,
					planes[p].length,
					PROT_READ | PROT_WRITE,
					MAP_SHARED,
					fd_,
					planes[p].m.mem_offset
				);

                if (buffers_[i].planes[p].start == MAP_FAILED) {
                    RKCAM_LOGE("mmap buffer=%u plane=%u failed: %s",
                               i,
                               p,
                               std::strerror(errno));
                    buffers_[i].planes[p].start = nullptr;
                    return false;
                }
				RKCAM_LOGI("mmap buffer[%u] plane[%u], length=%zu",
                           i,
                           p,
                           buffers_[i].planes[p].length);
			}

		}
		else{
			buffers_[i].planes.resize(1);

			buffers_[i].planes[0].length = buf.length;
			buffers_[i].planes[0].start = mmap(
				nullptr,
				buf.length,
				PROT_READ | PROT_WRITE,
				MAP_SHARED,
				fd_,
				buf.m.offset
			);
            if (buffers_[i].planes[0].start == MAP_FAILED) {
                RKCAM_LOGE("mmap buffer=%u failed: %s",
                           i,
                           std::strerror(errno));
                buffers_[i].planes[0].start = nullptr;
                return false;
            }

            RKCAM_LOGI("mmap buffer[%u], length=%zu",
                       i,
                       buffers_[i].planes[0].length);
		}

	}
	return true;
}

bool V4L2VideoSource::queueBuffer(uint32_t index){
	v4l2_buffer buf{};
	v4l2_plane planes[VIDEO_MAX_PLANES] {};

	buf.type = type_;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index = index;

	if(is_mplane_){
		buf.length = num_planes_;
		buf.m.planes = planes;
	}


	if(xioctl(fd_, VIDIOC_QBUF, &buf) < 0){
        RKCAM_LOGE("VIDIOC_QBUF index=%u failed: %s",
                   index,
                   std::strerror(errno));
        return false;
	}
	return true;
}

bool V4L2VideoSource::queueAllBuffer()
{
    for (uint32_t i = 0; i < buffers_.size(); ++i) {
        if (!queueBuffer(i)) {
            return false;
        }
    }	
    return true;
}
void V4L2VideoSource::cleanupBuffers(){
	for(auto& buffer : buffers_){
		for(auto& plane : buffer.planes){
			if(plane.start &&plane.start != MAP_FAILED){
				munmap(plane.start, plane.length);
				plane.start = nullptr;
				plane.length = 0;
			}
		}
	}
	buffers_.clear();
	buffer_in_user_.clear();
	if(fd_ > 0)
	{
		v4l2_requestbuffers req{};
		req.count = 0;
		req.type = type_;
		req.memory = V4L2_MEMORY_MMAP;
		if(xioctl(fd_, VIDIOC_REQBUFS, &req) < 0)
		{
			RKCAM_LOGE("VIDIOC_REQBUFS count=0 failed: %s",
                       std::strerror(errno));
		}
	}
	buffers_initialized_ = false;

}


 bool V4L2VideoSource::start(){
    if (!opened_) {
        RKCAM_LOGE("device not opened");
        return false;
    }

    if (streaming_) {
        return true;
    }
    if (!configured_) {
        RKCAM_LOGI("device not configured, configure now");

        if (!configure()) {
            RKCAM_LOGE("configure failed");
            return false;
        }
    }
	if (!buffers_initialized_) {
        if (!initMmap()) {
            RKCAM_LOGE("initMmap failed");
            cleanupBuffers();
            buffers_initialized_ = false;
            return false;
        }

        buffers_initialized_ = true;
    }

	if (!queueAllBuffer()) {
        RKCAM_LOGE("queueAllBuffer failed");

        /*
         * 如果 QBUF 中途失败，可能已经有部分 buffer 入队。
         * 这时不要保持半初始化状态，直接释放 buffer，下一次 start 重新来。
         */
        cleanupBuffers();
        buffers_initialized_ = false;
        return false;
    }
	v4l2_buf_type type = type_;
    if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        RKCAM_LOGE("VIDIOC_STREAMON failed: %s", std::strerror(errno));

        /*
         * STREAMON 失败后，buffer 可能已经 QBUF。
         * 为了保持状态干净，释放 buffer，下一次 start 重新 initMmap。
         */
        cleanupBuffers();
        buffers_initialized_ = false;
        return false;
    }
    streaming_ = true;
    RKCAM_LOGI("stream on");

    return true;
 }

V4L2VideoSource::WaitResult V4L2VideoSource::waitForFrame(int timeout_sec){
 	while (true) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd_, &fds);

        timeval tv {};
        tv.tv_sec = timeout_sec;
        tv.tv_usec = 0;

        int ret = select(fd_ + 1, &fds, nullptr, nullptr, &tv);

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }

            RKCAM_LOGE("select failed: %s", std::strerror(errno));
            return WaitResult::Error;
        }

        if (ret == 0) {
            RKCAM_LOGE("select timeout");
            return WaitResult::Timeout;
        }

        return WaitResult::Ready;
    }
}

bool V4L2VideoSource::readFrame(VideoFrame& frame)
{
	if(!streaming_){
		RKCAM_LOGE("stream is not on");
		return false;
	}

	while(true){
		auto wait_result = waitForFrame(2);
		if (wait_result != WaitResult::Ready) {
			return false;
		}
		v4l2_buffer buf {};
		v4l2_plane planes[VIDEO_MAX_PLANES] {};

		buf.type = type_;
		buf.memory = V4L2_MEMORY_MMAP;

		if(is_mplane_){
			buf.length = num_planes_;
			buf.m.planes = planes;
		}

		if(xioctl(fd_, VIDIOC_DQBUF, &buf) < 0){
			if (errno == EAGAIN) {
                continue;
            }
			RKCAM_LOGE("VIDIOC_DQBUF failed: %s", std::strerror(errno));
            return false;
		}
		if (buf.index >= buffers_.size()) {
            RKCAM_LOGE("invalid dequeued buffer index: %u", buf.index);
            return false;
        }

		if (buffer_in_user_[buf.index]) {
    		RKCAM_LOGE("buffer index=%u is already in user, internal state error", buf.index);
    		return false;
		}

		buffer_in_user_[buf.index] = true;

		frame.width = width_;
		frame.height = height_;
		frame.format = pixel_format_;
		frame.frame_id = frame_id_ ++;
		frame.buffer_index = static_cast<int>(buf.index);

		frame.pts_us = static_cast<uint64_t>(buf.timestamp.tv_sec) * 1000000LL + 
		static_cast<int64_t>(buf.timestamp.tv_usec);

		frame.planes.clear();
		if(is_mplane_){
			for(uint32_t p = 0; p < num_planes_; p++){
				VideoPlane plane;
				plane.data = buffers_[buf.index].planes[p].start;
				plane.length = buffers_[buf.index].planes[p].length;
				plane.bytesused = planes[p].bytesused;
				plane.stride = p < plane_strides_.size() ? plane_strides_[p] : 0;
				plane.dma_fd = -1;
				frame.planes.push_back(plane);
			}
		}
		else{
			VideoPlane plane;
			plane.data = buffers_[buf.index].planes[0].start;
			plane.length = buffers_[buf.index].planes[0].length;
			plane.bytesused = buf.bytesused;
			plane.stride = !plane_strides_.empty() ? plane_strides_[0] : 0;
			plane.dma_fd = -1;
			frame.planes.push_back(plane);
		}
		return true;
	}
}

bool V4L2VideoSource::releaseFrame(const VideoFrame& frame)
{
	if (frame.buffer_index < 0) {
        RKCAM_LOGE("invalid frame buffer_index=%d", frame.buffer_index);
        return false;
    }

    uint32_t index = static_cast<uint32_t>(frame.buffer_index);

    if (index >= buffers_.size()) {
        RKCAM_LOGE("releaseFrame invalid buffer index=%u", index);
        return false;
    }
	    /*
     * 如果已经 stop/streamoff，就不要再 QBUF 旧 frame。
     * stop() 后旧 frame 视为失效。
     */
    if (!streaming_) {
        RKCAM_LOGE("releaseFrame called while stream is not on, index=%u", index);
        return false;
    }

	if (index >= buffer_in_user_.size()) {
        RKCAM_LOGE("buffer_in_user_ size mismatch, index=%u", index);
        return false;
    }

 	if (!buffer_in_user_[index]) {
        RKCAM_LOGE("buffer index=%u is not owned by user, maybe double release", index);
        return false;
    }

    if (!queueBuffer(index)) {
        RKCAM_LOGE("queueBuffer failed in releaseFrame, index=%u", index);
        return false;
    }

    buffer_in_user_[index] = false;
    return true;

}



bool V4L2VideoSource::stop()
{
    if (!opened_) {
        return true;
    }

    if (!streaming_) {
        return true;
    }

    v4l2_buf_type type = type_;

    if (xioctl(fd_, VIDIOC_STREAMOFF, &type) < 0) {
        RKCAM_LOGE("VIDIOC_STREAMOFF failed: %s", std::strerror(errno));
        return false;
    }

    streaming_ = false;
    std::fill(buffer_in_user_.begin(), buffer_in_user_.end(), false);

    RKCAM_LOGI("stream off");
    return true;
}


void V4L2VideoSource::enumFrameSizes(uint32_t pixelfmt)
{
	for(uint32_t i = 0; ; i++)
	{
		v4l2_frmsizeenum frmsize {};
		frmsize.index = i;
		frmsize.pixel_format = pixelfmt;

		if(xioctl(fd_, VIDIOC_ENUM_FRAMESIZES, &frmsize) < 0)
		{
			break;
		}
		if(frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE)
		{
			RKCAM_LOGI("framesize[%u]	 %ux%u",
						i,
						frmsize.discrete.width,
						frmsize.discrete.height);
		}
		else if(frmsize.type == V4L2_FRMSIZE_TYPE_STEPWISE)
		{
			RKCAM_LOGI("framesize[%u] stepwise min=%ux%u max=%ux%u step=%ux%u",
						i,
						frmsize.stepwise.min_width,
						frmsize.stepwise.min_height,
						frmsize.stepwise.max_width,
						frmsize.stepwise.max_height,
						frmsize.stepwise.step_width,
						frmsize.stepwise.step_height);
		}
		else if(frmsize.type == V4L2_FRMSIZE_TYPE_CONTINUOUS)
		{
			RKCAM_LOGI("framesize[%u] continuous min=%ux%u max=%ux%u",
						i,
						frmsize.stepwise.min_width,
						frmsize.stepwise.min_height,
						frmsize.stepwise.max_width,
						frmsize.stepwise.max_height);
		}
	}
}

bool V4L2VideoSource::enumFormats(){
	RKCAM_LOGI("Enumerating formats:");

    for (uint32_t i = 0; ; ++i) {
        v4l2_fmtdesc desc {};
        desc.index = i;
        desc.type = type_;

        if (xioctl(fd_, VIDIOC_ENUM_FMT, &desc) < 0) {
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

        enumFrameSizes(desc.pixelformat);
    }

    return true;
}
bool V4L2VideoSource::close()
{
    if (streaming_) {
        stop();
    }

    if (buffers_initialized_) {
        cleanupBuffers();
    }

    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }

    opened_ = false;
    configured_ = false;
    streaming_ = false;
    buffers_initialized_ = false;

    return true;
}

}