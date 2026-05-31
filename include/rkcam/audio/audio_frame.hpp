/**
*@file include/rkcam/audio/audio_frame.hpp
*@brief 
*@author HKH368
*@date 2026-05-29
*@return 
*/

#pragma once
#include <cstdint>
#include <cstddef>

namespcae rkcam{
enum class SampleFormat{
	Unknown,
	S16,
	S32,
	Float,
};

struct AudioFrame{
	int sample_rate = 0;
	int channels = 0;
	int nb_samples = 0; //number of samples of one frame 1024

	int64_t pts_us = 0;
	int64_t duration_us = 0;
	int64_t frame_id = 0;

	void* data = nullptr;
	size_t size = 0;
};


}
