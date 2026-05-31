/**
*@file v4l2_video_source.cpp
*@brief 
*@author HKH368
*@date 2026-05-29
*@return 
*/

#include "rkcam/video/video_source.hpp"
#include "rkcam/core/log.hpp"

namespace rkcam{

class V4L2VideoSource : public IVideoSource{
public:
	bool open() override
	{
		RKCAM_LOGI("V4L2VideoSource open placeholder");
		return true;
	}
	bool start() override
	{
		RKCAM_LOGI("V4L2VideoSource start placeholder");
		return true;
	}
	bool readFrame(VideoFrame& Frame) override
	{
		RKCAM_LOGI("V4L2VideoSource readFrame placeholder");
        return false;
	}
	bool stop() override
    {
        RKCAM_LOGI("V4L2VideoSource stop placeholder");
        return true;
    }

    bool close() override
    {
        RKCAM_LOGI("V4L2VideoSource close placeholder");
        return true;
    }

};


}
