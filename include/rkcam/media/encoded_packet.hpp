/**
*@file include/rkcam/media/encoded_packet.hpp
*@brief 
*@author HKH368
*@date 2026-05-29
*@return 
*/

#pragma once
#include <cstdint>
#include <cstddef>

namespace rkcam{
enum class MediaType{
	Unknown,
	Video,
	Audio,
};
enum class CodecType{
	Unknown,
	H264,
	H265,
	MJPEG,
	AAC,
	OPUS,
	PCM,
};

struct EncodedPacket{
	MediaType media_type = MediaType::Unknown;
	CodecType codec_type = CodecType::Unknown;

	int64_t pts_us = 0; //
	int64_t dts_us = 0; //Decoding Time Stamp. only useful for Video.for Audio, dts_us = pts_us
	int64_t duration_us = 0;

	bool key_frame = false; //only useful for Video.
	void* data = nullptr;
	size_t size = 0;

};
}
