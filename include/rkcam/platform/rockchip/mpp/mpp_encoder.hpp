#pragma once

#include "rkcam/media/encoded_packet.hpp"
#include "rkcam/video/video_frame.hpp"
#include "rkcam/video/video_types.hpp"

#include "rkcam/platform/rockchip/mpp/mpp_rk3568/rk_mpi.h"
#include "rkcam/platform/rockchip/mpp/mpp_rk3568/mpp_frame.h"
#include "rkcam/platform/rockchip/mpp/mpp_rk3568/mpp_packet.h"

#include <cstddef>
#include <cstdint>

namespace rkcam{

struct MppEncoderConfig{
    int width = 0;
    int height = 0;

    PixelFormat input_format = PixelFormat::NV12;

    /*
     * MPP frame stride.
     *
     * hor_stride:
     *   MPP 使用的水平 sample/pixel stride。
     *
     *   当前 MppEncoder v1 只支持 NV12：
     *     NV12 Y sample = 1 byte
     *     所以数值上等于 VideoBufferPlane::stride。
     *
     * ver_stride:
     *   MPP 使用的垂直对齐行数。
     *
     * 如果为 0：
     *   hor_stride 从 frame.buffer->planes[0].stride 转换得到；
     *   ver_stride 从 frame.buffer->planes[0].height_stride 得到。
     */
    int hor_stride = 0;
    int ver_stride = 0;

    CodecType codec = CodecType::H264;

    int fps = 30;
    int gop = 30;

    /*
     * 码率，单位 bps。
     */
    int bitrate = 2 * 1000 * 1000;
};
    

class MppEncoder{
public:
    explicit MppEncoder(const MppEncoderConfig& config);
    ~MppEncoder();

    MppEncoder(const MppEncoder&) = delete;
    MppEncoder& operator=(const MppEncoder&) = delete;

    bool init();
    void close();

    bool encode(const PipelineVideoFrame& frame, EncodedPacket& packet);
private:
    bool setupEncoder();
    bool makeMppFrame(const PipelineVideoFrame& frame, MppFrame& mpp_frame);

    static MppCodingType toMppCodingType(CodecType codec);
    static MppFrameFormat toMppFrameFormat(PixelFormat format);
private:
    MppEncoderConfig config_;

    MppCtx ctx_ = nullptr;
    MppApi* mpi_ = nullptr;

    bool initialized_ =false;
};

}