#pragma once

#include "rkcam/core/blocking_queue.hpp"

#include <cstdint>
#include <vector>
#include <string>
#include <memory>
namespace rkcam{
enum class MediaType{
	Unknown,
	Video,
	Audio,
};
struct EncodedBuffer {
    std::vector<uint8_t> data;
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
	
	std::shared_ptr<EncodedBuffer> buffer;

	int64_t pts_us = 0;
	int64_t dts_us = 0;
	int64_t duration_us = 0;

	bool key_frame = false;
	bool eos = false;

	const uint8_t* data() const
    {
        return buffer && !buffer->data.empty()
            ? buffer->data.data()
            : nullptr;
    }
	size_t size() const
    {
        return buffer ? buffer->data.size() : 0;
    }
	bool empty() const
    {
        return !buffer || buffer->data.empty();
    }
	
};
struct EncodedPacketInputPort{
    std::string port_name;
    /*
     * 用于检查上游有没有接错。
     */
    std::string stream_id;
    MediaType media_type = MediaType::Unknown;
    CodecType codec = CodecType::Unknown;
    /*
     * 非拥有指针。
     * 队列生命周期必须长于 Mp4RecordStage。
     */
    BlockingQueue<EncodedPacket>* queue = nullptr;

    /*
     * 当前先保留。
     * 后面可用于“音频异常时是否允许仅视频继续录像”。
     */
    bool required = true;
};
}