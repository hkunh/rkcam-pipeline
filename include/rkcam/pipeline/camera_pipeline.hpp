#pragma once

#include "rkcam/core/blocking_queue.hpp"

#include "rkcam/pipeline/capture_stage.hpp"
#include "rkcam/pipeline/fps_stage.hpp"
#include "rkcam/pipeline/raw_save_stage.hpp"
#include "rkcam/pipeline/rga_stage.hpp"
#include "rkcam/pipeline/pipeline_stage.hpp"

#include "rkcam/video/video_frame.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace rkcam {

enum class StageType {
    Capture,
    Rga,
    Fps,
    RawSave,
};

struct PipelineQueueConfig{
    std::string name;
    size_t capacity = 4;
    QueueFullPolicy policy = QueueFullPolicy::DropOldest;
    bool valid() const
    {
        return !name.empty();
    }
};
struct StageNodeConfig{
    std::string name;
    StageType type = StageType::Capture;

    /*
     * input_queue:
     *   Capture 这种 source stage 可以为空。
     *
     * output_queue:
     *   RawSave / Fps 这种 sink stage 可以为空。
     */
    PipelineQueueConfig input_queue;
    PipelineQueueConfig output_queue;

    /*
    * 每个 stage 自己的配置。
    * 第一版不用 std::variant，简单直接。
    */
    CaptureStageConfig capture;
    RgaStageConfig rga;
    FpsStageConfig fps;
    RawSaveStageConfig raw_save;

};

struct StageNode{
    StageNodeConfig config;

    std::shared_ptr<BlockingQueue<PipelineVideoFrame>> input_queue;
    std::unique_ptr<IStage> stage;
    std::shared_ptr<BlockingQueue<PipelineVideoFrame>> output_queue;
};

struct CameraPipelineConfig {
    std::string stream_id = "cam0";
    /*
     * 按 上游 -> 下游 顺序填写。
     *
     * 例如：
     *   capture -> rga -> raw_save
     */
    std::vector<StageNodeConfig> nodes;
};
class CameraPipeline{
public: 
    explicit CameraPipeline(const CameraPipelineConfig& config);
    ~CameraPipeline();

    CameraPipeline(const CameraPipeline&) = delete;
    CameraPipeline& operator=(const CameraPipeline&) = delete;

    bool start();
    void stop();

    bool isRunning() const;
private:
    bool initStageNodes();
    bool initStageNodeQueues();
    bool initStageNodeStages();

    bool createOrReuseQueue(const PipelineQueueConfig& config, std::shared_ptr<BlockingQueue<PipelineVideoFrame>>& queue);

    bool createStageForNode(StageNode& node);

    bool startStages();
    void stopStages();

    void stopAllQueues();
    void clearAllQueues();

    void destroy();

private:
    CameraPipelineConfig config_;

    /*
     * 所有队列按 name 统一管理。
     * StageNode 里的 input_queue / output_queue 只是 shared_ptr 引用。
     */
     std::unordered_map<std::string, std::shared_ptr<BlockingQueue<PipelineVideoFrame>>> queue_map_;
     
    /*
    * 每个节点包含：
    *   input queue config + input queue
    *   stage
    *   output queue config + output queue
    */
    std::vector<StageNode> nodes_;

    std::atomic<bool> running_{false};
};

} // namespace rkcam