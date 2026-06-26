#pragma once

#include <cstdint>
#include <vector>
#include <string>

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

	PCM,
	AAC,
};
struct EncodedPacket{
	std::string stream_id = "cam0";
	MediaType media_type = MediaType::Unknown;
	CodecType codec = CodecType::Unknown;
	
	std::vector<uint8_t> data;

	int64_t pts_us = 0;
	int64_t dts_us = 0;
	int64_t duration_us = 0;

	bool key_frame = false;
	bool eos = false;

	
};

}