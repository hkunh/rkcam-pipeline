#pragma once

#include "rkcam/core/blocking_queue.hpp"
#include "rkcam/pipeline/pipeline_stage.hpp"
#include "rkcam/pipeline/pipeline_video_frame.hpp"

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <string>
#include <thread>

namespace rkcam {

struct RawSaveStageConfig {
    std::string stage_name = "raw_save";

    /*
     * 输出文件路径。
     * 注意：目录需要提前存在，例如 /userdata/rkcam/output/
     */
    std::string output_path = "/userdata/rkcam/output/capture_nv12.yuv";

    /*
     * 最多保存多少帧。
     * 0 表示不限制。
     */
    int max_frames = 100;

    /*
     * true:
     *   对 NV12 按 width/height/stride 逐行保存成紧凑格式，
     *   方便 ffplay -pixel_format nv12 -video_size WxH 播放。
     *
     * false:
     *   直接按每个 plane 的 bytesused 原样写出，
     *   会保留 stride padding。
     */
    bool save_tight_nv12 = true;

    /*
     * 保存到 max_frames 后是否停止队列。
     * 测试程序建议 true，这样保存完 100 帧后能让上游 CaptureStage 退出。
     */
    bool stop_queue_when_done = true;
};

class RawSaveStage : public IStage {
public:
    RawSaveStage(
        const RawSaveStageConfig& config,
        BlockingQueue<PipelineVideoFrame>& input_queue);

    ~RawSaveStage() override;

    RawSaveStage(const RawSaveStage&) = delete;
    RawSaveStage& operator=(const RawSaveStage&) = delete;

    bool start() override;
    void stop() override;

private:
    void threadLoop();

    bool openFile();
    void closeFile();

    bool writeFrame(const PipelineVideoFrame& frame);
    bool writeFrameRawLayout(const PipelineVideoFrame& frame);
    bool writeFrameTightNv12(const PipelineVideoFrame& frame);

private:
    RawSaveStageConfig config_;

    BlockingQueue<PipelineVideoFrame>& input_queue_;

    std::thread thread_;
    std::atomic<bool> running_{false};

    FILE* fp_ = nullptr;
    int saved_frames_ = 0;
};

} // namespace rkcam