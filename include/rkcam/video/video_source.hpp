/**
*@file include/rkcam/video/video_source.hpp
*@brief 
*@author HKH368
*@date 2026-05-29
*@return 
*/


#pragma once
#include "rkcam/video/video_frame.hpp"

namespace rkcam{
class IVideoSource{
public:
	virtual ~IVideoSource() = default;
	virtual bool open() = 0;
	virtual bool start() = 0;
	virtual bool readFrame(VideoFrame& frame) = 0;
	virtual bool stop() = 0;
	virtual bool close() = 0;
};
}
