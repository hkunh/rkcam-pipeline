#pragma once

#include "rkcam/core/blocking_queue.hpp"
#include "rkcam/media/encoded_packet.hpp"
#include "rkcam/pipeline/pipeline_stage.hpp"
#include "rkcam/video/video_frame.hpp"
#include "rkcam/platform/rockchip/mpp/mpp_encoder.hpp"

#include <atomic>
#include <thread>
#include <string>

namespace rkcam{

struct MppStageConfig{
    std::string stage_name = "mpp";

    /*
    * MPP 编码器配置。
    *
    * 注意：
    *   encoder.width / height 必须和输入到 MppStage 的 frame.width / height 一致。
    *   也就是 RgaStage 的输出尺寸。
    */
    MppEncoderConfig encoder;

};
class MppStage : public IStage{
public:
    MppStage(const MppStageConfig& config, BlockingQueue<PipelineVideoFrame>& input_queue, BlockingQueue<EncodedPacket>& output_queue);
    ~MppStage() override;

    MppStage(const MppStage&) = delete;
    MppStage& operator=(const MppStage&) = delete;

    bool start() override;
    void stop() override;
    /*
     * 只提交请求。
     * 真正的 MPP control 在编码线程中执行。
     */
    bool requestIdr(int64_t min_pts_us);
    /*
     * 获取当前编码器配置对应的 H264 SPS/PPS Annex-B header。
     */
    bool getCodecHeader(std::vector<uint8_t>& header);
private:
    void threadLoop();

private:
    MppStageConfig config_;

    BlockingQueue<PipelineVideoFrame>& input_queue_;
    BlockingQueue<EncodedPacket>& output_queue_;

    std::thread thread_;

    MppEncoder encoder_;

    std::atomic<bool> running_{false};

    int encoded_packets_ = 0;
    int failed_frames_ = 0;

    /*
    * -1：没有待处理 IDR 请求
    * >=0：所有待处理请求要求的最小 frame PTS
    */
    std::atomic<int64_t> pending_idr_min_pts_us_{-1};

    uint64_t requested_idr_count_ = 0;
    uint64_t applied_idr_count_ = 0;
    uint64_t failed_idr_count_ = 0;



};


}