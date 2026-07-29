#pragma once
#include "rkcam/audio/audio_types.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace rkcam{

struct AudioBuffer{
    /*
     * 第一版只做 CPU PCM。
     *
     * ALSA readi() 读出来的是 interleaved PCM：
     *   S16_LE stereo:
     *     L0 R0 L1 R1 L2 R2 ...
     */
	std::vector<uint8_t> data;
};
struct AudioFrame{
	std::string stream_id = "audio0";

	int sample_rate = 0;
	int channels = 0;
	AudioSampleFormat format = AudioSampleFormat::Unknown;

    /*
     * 每个声道的采样点数量。
     *
     * 例如：
     *   48kHz / stereo / S16_LE / nb_samples=1024
     *   data size = 1024 * 2 * 2 = 4096 bytes
     */
	int nb_samples = 0;

	int64_t pts_us = 0;
	int64_t duration_us = 0;
	int64_t frame_id = 0;

	std::shared_ptr<AudioBuffer> buffer;

	uint8_t* data()
	{
		if(!buffer || buffer->data.empty())
		{
			return nullptr;
		}
		return buffer->data.data();
	}

	const uint8_t* data() const
	{
        if (!buffer || buffer->data.empty()) {
            return nullptr;
        }

        return buffer->data.data();
	}

	size_t size() const
	{
		return buffer ? buffer->data.size() : 0;
	}
	bool empty()
	{
		return size() == 0;
	}

};


/*
 * 先直接复用 AudioFrame。
 * 后面如果你想区分 source frame / pipeline frame，再拆。
 */
using AudioSourceFrame = AudioFrame;
using PipelineAudioFrame = AudioFrame;



}//namespace rkcam