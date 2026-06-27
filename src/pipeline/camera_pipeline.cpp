#include "rkcam/pipeline/camera_pipeline.hpp"
#include "rkcam/core/log.hpp"
#include "rkcam/video/v4l2_video_source.hpp"

#include <memory>
#include <utility>

namespace rkcam {
CameraPipeline::CameraPipeline(const CameraPipelineConfig& config)
    : config_(config)
{
}

CameraPipeline::~CameraPipeline()
{
    stop();
}
bool CameraPipeline::start()
{
    if(running_)
    {
        return true;
    }
    if(!initStageNodes())
    {
        RKCAM_LOGE("[%s] initStageNodes failed",
            config_.stream_id.c_str());
        destroy();
        return false;
    }
    if(!startStages())
    {
        RKCAM_LOGE("[%s] startStages failed",
            config_.stream_id.c_str());
        stop();
        return false;
    }
    running_ = true;

    RKCAM_LOGI("[%s] CameraPipeline started",
               config_.stream_id.c_str());

    return true;
}

void CameraPipeline::stop()
{
    if (!running_ && nodes_.empty() && queue_map_.empty()) {
        return;
    }

    running_ = false;

    /*
     * 停 stage，再停 queue，再清理残留 frame。
     */
    stopStages();
    stopAllQueues();
    clearAllQueues();

    destroy();

    RKCAM_LOGI("[%s] CameraPipeline stopped",
               config_.stream_id.c_str());
}

bool CameraPipeline::isRunning() const
{
    if(!running_)
    {
        return true;
    }
    /*
    * 所有队列 stopped 且 empty，则认为自然结束。
    */
    bool all_done = true;
    for(const auto& kv : queue_map_){
        const auto& queue = kv.second;
        if(!queue)
        {
            continue;
        }
        if(!queue->stopped() || queue->size() != 0)
        {
            all_done = false;
            break;
        }
    }
    return !all_done;
}

bool CameraPipeline::initStageNodes(){
    nodes_.clear();
    queue_map_.clear();

    /*
    * 先把配置拷贝成运行时 StageNode。
    */
    for(const auto& cfg : config_.nodes){
        StageNode node;
        node.config = cfg;
        nodes_.push_back(std::move(node));
    }

    if(!initStageNodeQueues())
    {
        return false;
    }
    if(!initStageNodeStages())
    {
        return false;
    }
    return true;

}
bool CameraPipeline::initStageNodeQueues()
{
    for(auto& node : nodes_)
    {
        /*
        * 初始化 input queue。
        * Capture 这类 source stage 可以没有 input queue。
        */
        if(node.config.input_queue.valid())
        {
            if(!createOrReuseQueue(node.config.input_queue, node.input_queue)){
                RKCAM_LOGE("[%s] create input queue failed, stage=%s queue=%s",
                           config_.stream_id.c_str(),
                           node.config.name.c_str(),
                           node.config.input_queue.name.c_str());
                return false;
            }
        }

        /*
        * 初始化 output queue。
        * RawSave / Fps 这类 sink stage 可以没有 output queue。
        */
        if(node.config.output_queue.valid())
        {
            if(!createOrReuseQueue(node.config.output_queue, node.output_queue)){
                RKCAM_LOGE("[%s] create output queue failed, stage=%s queue=%s",
                           config_.stream_id.c_str(),
                           node.config.name.c_str(),
                           node.config.output_queue.name.c_str());
                return false;
            }
        }

    }
    return true;
}

bool CameraPipeline::createOrReuseQueue(
    const PipelineQueueConfig& config,
    std::shared_ptr<BlockingQueue<PipelineVideoFrame>>& queue)
{
    if (!config.valid()) {
        queue.reset();
        return true;
    }

    auto it = queue_map_.find(config.name);
    if(it != queue_map_.end())
    {
        /*
        * 已存在同名队列，直接复用。
        * 这样前一个 node 的 output_queue_ 和后一个 node 的 input_queue_
        * 会指向同一个 BlockingQueue。
        */
        queue = it -> second;
        return true;
    }
    auto q = std::make_shared<BlockingQueue<PipelineVideoFrame>>(config.capacity, config.policy);
    queue = q;
    queue_map_.emplace(config.name, std::move(q));
    RKCAM_LOGI("[%s] create queue: %s capacity=%zu",
            config_.stream_id.c_str(),
            config.name.c_str(),
            config.capacity);

    return true;
}   
bool CameraPipeline::initStageNodeStages()
{
    for(auto& node : nodes_)
    {
        if(!createStageForNode(node))
        {
            RKCAM_LOGE("[%s] create stage failed: %s",
                       config_.stream_id.c_str(),
                       node.config.name.c_str());
            return false;
        }
    }
    return true;
}
bool CameraPipeline::createStageForNode(StageNode& node)
{
    if (node.config.name.empty()) {
        RKCAM_LOGE("[%s] stage name is empty",
                   config_.stream_id.c_str());
        return false;
    }
    switch(node.config.type){
        case StageType::Capture:{
            if(!node.output_queue)
            {
                RKCAM_LOGE("[%s] CaptureStage %s requires output_queue",
                       config_.stream_id.c_str(),
                       node.config.name.c_str());
                return false;
            }
            CaptureStageConfig capture_cfg = node.config.capture;
            if (capture_cfg.stream_id.empty()) {
                capture_cfg.stream_id = config_.stream_id;
            }
            std::unique_ptr<IVideoSource> source =std::make_unique<V4L2VideoSource>(capture_cfg.source);
            node.stage = std::make_unique<CaptureStage>(capture_cfg, std::move(source), *node.output_queue);
            RKCAM_LOGI("[%s] create CaptureStage: %s -> %s",
                    config_.stream_id.c_str(),
                    node.config.name.c_str(),
                    node.config.output_queue.name.c_str());

            return true;
            
        }
        case StageType::Rga:{
            if(!node.input_queue || !node.output_queue)
            {
                 RKCAM_LOGE("[%s] RgaStage %s requires input_queue and output_queue",
                        config_.stream_id.c_str(),
                        node.config.name.c_str());
                return false;
            }
            RgaStageConfig rga_cfg = node.config.rga;
            if (rga_cfg.stage_name.empty()) {
                rga_cfg.stage_name = node.config.name;
            }
            node.stage = std::make_unique<RgaStage>(
                rga_cfg,
                *node.input_queue,
                *node.output_queue);
            RKCAM_LOGI("[%s] create RgaStage: %s %s -> %s",
                   config_.stream_id.c_str(),
                   node.config.name.c_str(),
                   node.config.input_queue.name.c_str(),
                   node.config.output_queue.name.c_str());

            return true;
        }
        case StageType::Fps: {
            if (!node.input_queue) {
                RKCAM_LOGE("[%s] FpsStage %s requires input_queue",
                        config_.stream_id.c_str(),
                        node.config.name.c_str());
                return false;
            }

            FpsStageConfig fps_cfg = node.config.fps;

            if (fps_cfg.stage_name.empty()) {
                fps_cfg.stage_name = node.config.name;
            }

            node.stage = std::make_unique<FpsStage>(
                fps_cfg,
                *node.input_queue);

            RKCAM_LOGI("[%s] create FpsStage: %s <- %s",
                    config_.stream_id.c_str(),
                    node.config.name.c_str(),
                    node.config.input_queue.name.c_str());

            return true;
        }
        case StageType::RawSave: {
            if (!node.input_queue) {
                RKCAM_LOGE("[%s] RawSaveStage %s requires input_queue",
                        config_.stream_id.c_str(),
                        node.config.name.c_str());
                return false;
            }

            RawSaveStageConfig save_cfg = node.config.raw_save;

            if (save_cfg.stage_name.empty()) {
                save_cfg.stage_name = node.config.name;
            }

            node.stage = std::make_unique<RawSaveStage>(
                save_cfg,
                *node.input_queue);

            RKCAM_LOGI("[%s] create RawSaveStage: %s <- %s",
                    config_.stream_id.c_str(),
                    node.config.name.c_str(),
                    node.config.input_queue.name.c_str());

            return true;
        }
        default:
            RKCAM_LOGE("[%s] unsupported stage type, stage=%s",
                   config_.stream_id.c_str(),
                   node.config.name.c_str());
            return false;
    }

}

bool CameraPipeline::startStages()
{
    /*
     * nodes_ 按 上游 -> 下游 配置。
     * 启动时反过来：下游先启动，上游后启动。
     */
     for(auto it = nodes_.rbegin(); it != nodes_.rend(); it++){
        if(!it->stage)
        {
            RKCAM_LOGE("[%s] stage is null: %s",
                       config_.stream_id.c_str(),
                       it->config.name.c_str());
            return false;
        }
        if(!it->stage->start())
        {
            RKCAM_LOGE("[%s] stage start failed: %s",
                       config_.stream_id.c_str(),
                       it->config.name.c_str());
            return false;
        }
     }
     return true;
}

void CameraPipeline::stopStages()
{
    for(auto& node : nodes_)
    {
        if(node.stage)
        {
            if(node.stage)
            {
                node.stage->stop();
            }
        }
    }
}

void CameraPipeline::stopAllQueues()
{
    for(auto& kv : queue_map_)
    {
        if(kv.second)
        {
            kv.second->stop();
        }
    }
}

void CameraPipeline::clearAllQueues(){
    for(auto& kv : queue_map_)
    {
        if (kv.second) {
            kv.second->clear();
        }
    }
}

void CameraPipeline::destroy(){
    for(auto& node : nodes_)
    {
        if(node.stage){
            node.stage->stop();
            node.stage.reset();
        }
        node.input_queue.reset();
        node.output_queue.reset();
    }
    nodes_.clear();
    clearAllQueues();
    queue_map_.clear();
}



} // namespace rkcam