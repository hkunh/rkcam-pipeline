#include "rkcam/platform/rockchip/mpp/mpp_encoder.hpp"
#include "rkcam/platform/rockchip/mpp/mpp_buffer_pool.hpp"
#include "rkcam/core/log.hpp"

#include "rkcam/platform/rockchip/mpp/mpp_rk3568/rk_venc_cmd.h"
#include "rkcam/platform/rockchip/mpp/mpp_rk3568/rk_venc_cfg.h"


#include <algorithm>
#include <cstring>
#include <unistd.h>

namespace rkcam{

namespace{

int alignTo(int value, int align)
{
    if (value <= 0 || align <= 0) {
        return 0;
    }

    return (value + align - 1) / align * align;
}

int defaultMppHorStride(PixelFormat format, int width)
{
    if (width <= 0) {
        return 0;
    }

    switch (format) {
    case PixelFormat::NV12:
        /*
         * 当前 v1 只支持 NV12。
         * MPP 官方 demo 通常使用 align(width, 16) 作为 hor_stride。
         * 对 NV12 来说，数值也等于 Y plane bytes per line。
         */
        return alignTo(width, 16);

    default:
        return 0;
    }
}

int defaultMppVerStride(PixelFormat format, int height)
{
    if (height <= 0) {
        return 0;
    }

    switch (format) {
    case PixelFormat::NV12:
        return alignTo(height, 16);

    default:
        return 0;
    }
}

bool getMppPacketKeyFrame(MppPacket packet, bool& key_frame)
{
    key_frame = false;

    if (!packet) {
        return false;
    }

    MppMeta meta = mpp_packet_get_meta(packet);
    if (!meta) {
        return false;
    }

    RK_S32 is_intra = 0;
    MPP_RET ret = mpp_meta_get_s32(meta, KEY_OUTPUT_INTRA, &is_intra);
    if (ret != MPP_OK) {
        return false;
    }

    key_frame = (is_intra != 0);
    return true;
}

}

MppEncoder::MppEncoder(const MppEncoderConfig& config)
    : config_(config)
{
}

MppEncoder::~MppEncoder()
{
    close();
}
bool MppEncoder::init()
{
    if (initialized_) {
        return true;
    }

    if (config_.width <= 0 || config_.height <= 0) {
        RKCAM_LOGE("MppEncoder init failed: invalid size %dx%d",
                   config_.width,
                   config_.height);
        return false;
    }

    if (config_.input_format != PixelFormat::NV12) {
        RKCAM_LOGE("MppEncoder init failed: only NV12 is supported now, format=%d",
                   static_cast<int>(config_.input_format));
        return false;
    }

    const MppCodingType coding_type = toMppCodingType(config_.codec);
    if (coding_type == MPP_VIDEO_CodingUnused) {
        RKCAM_LOGE("MppEncoder init failed: unsupported codec=%d",
                   static_cast<int>(config_.codec));
        return false;
    }

    if (mpp_check_support_format(MPP_CTX_ENC, coding_type) != MPP_OK)
    {
        RKCAM_LOGE("MppEncoder init failed: codec is not supported by MPP, codec=%d coding=%d",
                   static_cast<int>(config_.codec),
                   static_cast<int>(coding_type));
        return false;
    }

    MPP_RET ret = mpp_create(&ctx_, &mpi_);
    if (ret != MPP_OK || !ctx_ || !mpi_) {
        RKCAM_LOGE("mpp_create failed, ret=%d ctx=%p mpi=%p",
                   ret,
                   ctx_,
                   mpi_);
        close();
        return false;
    }

    ret = mpp_init(ctx_, MPP_CTX_ENC, coding_type);
    if (ret != MPP_OK) {
        RKCAM_LOGE("mpp_init encoder failed, ret=%d coding=%d",
                   ret,
                   static_cast<int>(coding_type));
        close();
        return false;
    }
    if (!setupEncoder()) {
        RKCAM_LOGE("MppEncoder setupEncoder failed");
        close();
        return false;
    }
    initialized_ = true;

    RKCAM_LOGI("MppEncoder init success: %dx%d fmt=%d codec=%d fps=%d gop=%d bitrate=%d",
               config_.width,
               config_.height,
               static_cast<int>(config_.input_format),
               static_cast<int>(config_.codec),
               config_.fps,
               config_.gop,
               config_.bitrate);

    return true;

}

void MppEncoder::close()
{
    if(mpi_ && ctx_)
    {
        mpi_->reset(ctx_);
    }
    if(ctx_)
    {
        mpp_destroy(ctx_);
        ctx_ = nullptr;
    }
    mpi_ = nullptr;
    initialized_ =false;
}

bool MppEncoder::setupEncoder()
{
    if(!ctx_ || !mpi_)
    {
        RKCAM_LOGE("setupEncoder failed: ctx or mpi is null");
        return false;
    }
    const int hor_stride = config_.hor_stride > 0
        ? config_.hor_stride
        : defaultMppHorStride(config_.input_format, config_.width);

    const int ver_stride = config_.ver_stride > 0
        ? config_.ver_stride
        : defaultMppVerStride(config_.input_format, config_.height);
    if (hor_stride <= 0 || ver_stride <= 0) {
        RKCAM_LOGE("setupEncoder failed: invalid stride hor=%d ver=%d",
                   hor_stride,
                   ver_stride);
        return false;
    }

    const MppCodingType coding_type = toMppCodingType(config_.codec);
    const MppFrameFormat frame_format = toMppFrameFormat(config_.input_format);

    if (coding_type == MPP_VIDEO_CodingUnused) {
        RKCAM_LOGE("setupEncoder failed: unsupported codec=%d",
                   static_cast<int>(config_.codec));
        return false;
    }

    if (frame_format == MPP_FMT_BUTT) {
        RKCAM_LOGE("setupEncoder failed: unsupported input format=%d",
                   static_cast<int>(config_.input_format));
        return false;
    }

    /*
     * 1. 输入图像配置。
     */
    MppEncPrepCfg prep_cfg;
     std::memset(&prep_cfg, 0, sizeof(prep_cfg));

    prep_cfg.change = MPP_ENC_PREP_CFG_CHANGE_INPUT | MPP_ENC_PREP_CFG_CHANGE_FORMAT;

    prep_cfg.width = config_.width;
    prep_cfg.height = config_.height;
    prep_cfg.hor_stride = hor_stride;
    prep_cfg.ver_stride = ver_stride;
    prep_cfg.format = frame_format;
    prep_cfg.rotation = MPP_ENC_ROT_0;
    prep_cfg.mirroring = 0;

    MPP_RET ret = mpi_->control(ctx_, MPP_ENC_SET_PREP_CFG, &prep_cfg);
    if (ret != MPP_OK) {
        RKCAM_LOGE("MPP_ENC_SET_PREP_CFG failed, ret=%d", ret);
        return false;
    }

    /*
     * 2. 码率控制配置。
     * 第一版使用 CBR，方便验证稳定性。
     */
    const int fps = config_.fps > 0 ? config_.fps : 30;
    const int gop = config_.gop > 0 ? config_.gop : fps;
    const int bitrate = config_.bitrate > 0 ? config_.bitrate : 2 * 1000 * 1000;

    MppEncRcCfg rc_cfg;
    std::memset(&rc_cfg, 0, sizeof(rc_cfg));

    rc_cfg.change = MPP_ENC_RC_CFG_CHANGE_RC_MODE |
                    MPP_ENC_RC_CFG_CHANGE_BPS |
                    MPP_ENC_RC_CFG_CHANGE_FPS_IN |
                    MPP_ENC_RC_CFG_CHANGE_FPS_OUT |
                    MPP_ENC_RC_CFG_CHANGE_GOP |
                    MPP_ENC_RC_CFG_CHANGE_QP_RANGE |
                    MPP_ENC_RC_CFG_CHANGE_QP_RANGE_I |
                    MPP_ENC_RC_CFG_CHANGE_QP_MAX_STEP;
    rc_cfg.rc_mode = MPP_ENC_RC_MODE_CBR;
    rc_cfg.quality = MPP_ENC_RC_QUALITY_MEDIUM;
    
    rc_cfg.bps_target = bitrate;
    rc_cfg.bps_max = bitrate * 17 / 16;
    rc_cfg.bps_min = bitrate * 15 /16;

    rc_cfg.fps_in_flex = 0;
    rc_cfg.fps_in_num = fps;
    rc_cfg.fps_in_denorm = 1;

    rc_cfg.fps_out_flex = 0;
    rc_cfg.fps_out_num = fps;
    rc_cfg.fps_out_denorm = 1;

    rc_cfg.gop = gop;
    rc_cfg.skip_cnt = 0;
    rc_cfg.max_reenc_times = 0;

    rc_cfg.drop_mode = MPP_ENC_RC_DROP_FRM_DISABLED;
    rc_cfg.drop_threshold = 0;
    rc_cfg.drop_gap = 0;

    rc_cfg.super_mode = MPP_ENC_RC_SUPER_FRM_NONE;
    rc_cfg.rc_priority = MPP_ENC_RC_BY_BITRATE_FIRST;

    rc_cfg.qp_init = -1;
    rc_cfg.qp_min = 10;
    rc_cfg.qp_max = 50;
    rc_cfg.qp_min_i = 10;
    rc_cfg.qp_max_i = 51;
    rc_cfg.qp_max_step = 4;
    rc_cfg.qp_delta_ip = 2;
    ret = mpi_->control(ctx_, MPP_ENC_SET_RC_CFG, &rc_cfg);
       if (ret != MPP_OK) {
        RKCAM_LOGE("MPP_ENC_SET_RC_CFG failed, ret=%d", ret);
        return false;
    }
 
    /*
     * 3. 编码协议配置。
     * 第一版优先跑 H264。
     */
    MppEncCodecCfg codec_cfg;
    std::memset(&codec_cfg, 0, sizeof(codec_cfg));
    codec_cfg.coding = coding_type;
    switch(config_.codec){
        case CodecType::H264: {
           codec_cfg.h264.change = MPP_ENC_H264_CFG_STREAM_TYPE |
                                   MPP_ENC_H264_CFG_CHANGE_PROFILE |
                                   MPP_ENC_H264_CFG_CHANGE_ENTROPY |
                                   MPP_ENC_H264_CFG_CHANGE_TRANS_8x8 |
                                   MPP_ENC_H264_CFG_CHANGE_QP_LIMIT |
                                   MPP_ENC_H264_CFG_CHANGE_QP_LIMIT_I |
                                   MPP_ENC_H264_CFG_CHANGE_MAX_QP_STEP |
                                   MPP_ENC_H264_CFG_CHANGE_QP_DELTA;
            /*
            * 0: Annex-B。裸 .h264 文件建议用 Annex-B。
            */
            codec_cfg.h264.stream_type = 0;

            codec_cfg.h264.profile = 100;
            codec_cfg.h264.level = 40;

            /*
            * CABAC。
            */
            codec_cfg.h264.entropy_coding_mode = 1;// 开启高级的 CABAC，白嫖 15% 的压缩率，让 2M 码率发挥出最大的画质
            codec_cfg.h264.cabac_init_idc = 0;// 使用 0 号初始预设经验包，这是工业上最标准的默认配置

            codec_cfg.h264.transform8x8_mode = 1;

            codec_cfg.h264.qp_init = -1;
            codec_cfg.h264.qp_min = 10;
            codec_cfg.h264.qp_max = 51;
            codec_cfg.h264.qp_min_i = 10;
            codec_cfg.h264.qp_max_i = 51;
            codec_cfg.h264.qp_max_step = 4;
            codec_cfg.h264.qp_delta_ip = 2;

            codec_cfg.h264.deblock_disable = 0;
            codec_cfg.h264.deblock_offset_alpha = 0;
            codec_cfg.h264.deblock_offset_beta = 0;
            break;
        }
        default:
            RKCAM_LOGE("setupEncoder failed: unsupported codec=%d",
                    static_cast<int>(config_.codec));
            return false;
    }

    ret = mpi_->control(ctx_, MPP_ENC_SET_CODEC_CFG, &codec_cfg);
    if (ret != MPP_OK) {
        RKCAM_LOGE("MPP_ENC_SET_CODEC_CFG failed, ret=%d", ret);
        return false;
    }

    RKCAM_LOGI("MppEncoder setup done: visible=%dx%d stride=%dx%d codec=%d bitrate=%d fps=%d gop=%d",
               config_.width,
               config_.height,
               hor_stride,
               ver_stride,
               static_cast<int>(config_.codec),
               bitrate,
               fps,
               gop);

    return true;
}

bool MppEncoder::encode(
    const PipelineVideoFrame& frame,
    EncodedPacket& packet)
{
    if (!initialized_ || !ctx_ || !mpi_) {
        RKCAM_LOGE("MppEncoder encode failed: encoder is not initialized");
        return false;
    }
    packet = EncodedPacket{};
    MppFrame mpp_frame = nullptr;
    if (!makeMppFrame(frame, mpp_frame)) {
        RKCAM_LOGE("MppEncoder encode failed: makeMppFrame failed");
        return false;
    }

    MppPacket mpp_packet = nullptr;
    MPP_RET ret = mpi_->encode(ctx_, mpp_frame, &mpp_packet);
    /*
     * mpp_frame 只是 MppFrame wrapper。
     * 真正的 MppBuffer 生命周期由 PipelineVideoFrame::buffer 持有。
     */
    if (mpp_frame) {
        mpp_frame_deinit(&mpp_frame);
        mpp_frame = nullptr;
    }

    if (ret != MPP_OK) {
        RKCAM_LOGE("MppEncoder encode failed: mpi->encode ret=%d", ret);
        return false;
    }

    if (!mpp_packet) {
        RKCAM_LOGE("MppEncoder encode failed: output packet is null");
        return false;
    }



    /*
     * 2. 再主动取包。
     *
     * 有些 MPP 版本不是 put 后立刻有 packet，
     * 所以这里短暂轮询一下。
     */
    MppPacket mpp_packet = nullptr;

    constexpr int kMaxRetry = 100;
    constexpr int kSleepUs = 1000;

    for (int i = 0; i < kMaxRetry; ++i) {
        ret = mpi_->encode_get_packet(ctx_, &mpp_packet);

        if (ret != MPP_OK) {
            RKCAM_LOGE("MppEncoder encode_get_packet failed, ret=%d frame_id=%lld retry=%d",
                       ret,
                       static_cast<long long>(frame.frame_id),
                       i);
            return false;
        }

        if (mpp_packet) {
            break;
        }

        usleep(kSleepUs);
    }

    if (!mpp_packet) {
        RKCAM_LOGE("MppEncoder encode failed: timeout waiting packet, frame_id=%lld",
                   static_cast<long long>(frame.frame_id));
        return false;
    }





    void* pos = mpp_packet_get_pos(mpp_packet);
    const size_t length = mpp_packet_get_length(mpp_packet);
    if (!pos || length == 0) {
        RKCAM_LOGE("MppEncoder encode failed: invalid packet pos=%p length=%zu",
                   pos,
                   length);
        mpp_packet_deinit(&mpp_packet);
        return false;
    }

    const uint8_t* src = static_cast<const uint8_t*>(pos);

    packet.stream_id = frame.stream_id;
    packet.media_type = MediaType::Video;
    packet.codec = config_.codec;


    packet.data.assign(src, src + length);

    /*
    * 优先用 MPP packet 自己的 pts/dts。
    * 如果无效，再退回输入 frame 的 pts。
    */
    const RK_S64 pkt_pts = mpp_packet_get_pts(mpp_packet);
    const RK_S64 pkt_dts = mpp_packet_get_dts(mpp_packet);

    packet.pts_us = pkt_pts >= 0 ? pkt_pts : frame.pts_us;
    packet.dts_us = pkt_dts >= 0 ? pkt_dts : packet.pts_us;
    packet.duration_us = frame.duration_us;

    bool key_frame = false;
    if (!getMppPacketKeyFrame(mpp_packet, key_frame)) {
        /*
         * 不要在这里手动扫描 NAL。
         * 第一版拿不到 meta 就先认为不是关键帧。
         */
        RKCAM_LOGE("MppEncoder warning: failed to get KEY_OUTPUT_INTRA from packet meta");
        key_frame = false;
    }
    packet.key_frame = key_frame;
    packet.eos = mpp_packet_get_eos(mpp_packet) != 0;

    mpp_packet_deinit(&mpp_packet);
    return true;
}
bool MppEncoder::makeMppFrame(
    const PipelineVideoFrame& frame,
    MppFrame& mpp_frame)
{
    mpp_frame = nullptr;

    if (!frame.buffer) {
        RKCAM_LOGE("makeMppFrame failed: frame.buffer is null");
        return false;
    }
    if (frame.buffer->memory_type != VideoMemoryType::DmaBuffer) {
        RKCAM_LOGE("makeMppFrame failed: only DmaBuffer input is supported now, memory=%d",
                   static_cast<int>(frame.buffer->memory_type));
        return false;
    }
    if (frame.buffer->planes.size() != 1) {
        RKCAM_LOGE("makeMppFrame failed: only single-plane NV12 is supported now, planes=%zu",
                   frame.buffer->planes.size());
        return false;
    }

    const auto& plane = frame.buffer->planes[0];
    /*
     * 当前推荐链路：
     *   RGA -> MppBufferPool -> MppEncoder
     *
     * 所以这里要求 native_handle 里有 MppBuffer。
     */
    auto native = std::dynamic_pointer_cast<MppNativeBuffer>(frame.buffer->native_handle);
    if (!native || !native->buffer) {
        RKCAM_LOGE("makeMppFrame failed: input frame is not backed by MppBuffer");
        return false;
    }
    const int hor_stride = config_.hor_stride > 0 ? config_.hor_stride : plane.stride;
    const int ver_stride = config_.ver_stride > 0 ? config_.ver_stride : plane.height_stride;

    if (hor_stride <= 0 || ver_stride <= 0) {
        RKCAM_LOGE("makeMppFrame failed: invalid mpp stride hor=%d ver=%d",
                   hor_stride,
                   ver_stride);
        return false;
    }

    MPP_RET ret = mpp_frame_init(&mpp_frame);
    if (ret != MPP_OK || !mpp_frame) {
        RKCAM_LOGE("mpp_frame_init failed, ret=%d frame=%p",
                   ret,
                   mpp_frame);
        mpp_frame = nullptr;
        return false;
    }

    mpp_frame_set_width(mpp_frame, frame.width);
    mpp_frame_set_height(mpp_frame, frame.height);

    mpp_frame_set_hor_stride(mpp_frame, hor_stride);
    mpp_frame_set_ver_stride(mpp_frame, ver_stride);

    mpp_frame_set_fmt(mpp_frame, toMppFrameFormat(frame.format));
    /*
    * 核心：把 MppBuffer 设置给 MppFrame。
    */
    mpp_frame_set_buffer(mpp_frame, native->buffer);

    mpp_frame_set_pts(mpp_frame, frame.pts_us);

    return true;

}

MppCodingType MppEncoder::toMppCodingType(CodecType codec)
{
    switch (codec) {
    case CodecType::H264:
        return MPP_VIDEO_CodingAVC;

    case CodecType::H265:
        return MPP_VIDEO_CodingHEVC;

    case CodecType::MJPEG:
        return MPP_VIDEO_CodingMJPEG;

    default:
        return MPP_VIDEO_CodingUnused;
    }
}

MppFrameFormat MppEncoder::toMppFrameFormat(PixelFormat format)
{
    switch (format) {
    case PixelFormat::NV12:
        return MPP_FMT_YUV420SP;

    default:
        return MPP_FMT_BUTT;
    }
}



}