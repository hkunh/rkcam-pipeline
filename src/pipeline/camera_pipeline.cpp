#include "rkcam/pipeline/camera_pipeline.hpp"
#include "rkcam/core/log.hpp"
#include "rkcam/video/v4l2_video_source.hpp"

#include <memory>
#include <utility>

namespace rkcam {

namespace{
template <typename T, PipelineQueueValueType TypeValue>
rkcam::BlockingQueue<T>* getTypeQueue(
    const std::shared_ptr<rkcam::IPipelineQueue>& queue,
    const std::string& stage_name){
    
    if(!queue)
    {
        RKCAM_LOGE("[%s] queue is null",
            stage_name.c_str());
        return nullptr;
    }
    if(queue->valueType() != TypeValue)
    {
        RKCAM_LOGE("[%s] queue type mismatch, expected=%d actual=%d",
                   stage_name.c_str(),
                   static_cast<int>(TypeValue),
                   static_cast<int>(queue->valueType()));
        return nullptr;
    }
    auto typed = std::dynamic_pointer_cast<rkcam::PipelineQueueBox<T, TypeValue>>(queue);
    if (!typed) {
        RKCAM_LOGE("[%s] queue cast failed",
                   stage_name.c_str());
        return nullptr;
    }
    return &typed->queue();
}

}



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
     * 1. 停止 stage。
     *    stage 自己负责 drain 输入队列，并 stop 输出队列。
     */
    stopStages();

    /*
     * 2. 兜底停止所有 queue。
     *    正常情况下，各 stage 已经 stop 了相关 queue。
     *    这里主要处理异常路径。
     */
    stopAllQueues();

    /*
     * 3. 清理残留数据，释放还留在 queue 里的 buffer / packet。
     */
    clearAllQueues();

    /*
     * 4. 释放 stage / queue 引用。
     *    destroy() 不再调用 stop/clear。
     */
    destroy();

    RKCAM_LOGI("[%s] CameraPipeline stopped",
               config_.stream_id.c_str());
}

bool CameraPipeline::isRunning() const
{
    if(!running_)
    {
        return false;
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
    std::shared_ptr<IPipelineQueue>& queue)
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
        if (it->second->valueType() != config.value_type) {
            RKCAM_LOGE("[%s] queue type mismatch: name=%s old=%d new=%d",
                       config_.stream_id.c_str(),
                       config.name.c_str(),
                       static_cast<int>(it->second->valueType()),
                       static_cast<int>(config.value_type));
            return false;
        }

        queue = it->second;
        return true;
    }
    std::shared_ptr<IPipelineQueue> new_queue;
    switch(config.value_type)
    {
        case PipelineQueueValueType::PipelineVideoFrame:
            new_queue = std::make_shared<VideoFrameQueueBox>(config.capacity, config.policy);
            break;
        case PipelineQueueValueType::EncodedPacket:
            new_queue = std::make_shared<EncodedPacketQueueBox>(
                config.capacity,
                config.policy);
            break;

        default:
            RKCAM_LOGE("[%s] unsupported queue value type=%d, name=%s",
                    config_.stream_id.c_str(),
                    static_cast<int>(config.value_type),
                    config.name.c_str());
            return false;
    }
    queue = new_queue;
    queue_map_.emplace(config.name, new_queue);
    RKCAM_LOGI("[%s] create queue: %s type=%d capacity=%zu",
            config_.stream_id.c_str(),
            config.name.c_str(),
            static_cast<int>(config.value_type),
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
            auto* out_q = getTypeQueue<PipelineVideoFrame, PipelineQueueValueType::PipelineVideoFrame>(
                node.output_queue,
                node.config.name
            );
            if (!out_q) {
                RKCAM_LOGE("[%s] CaptureStage %s requires VideoFrame output_queue",
                        config_.stream_id.c_str(),
                        node.config.name.c_str());
                return false;
            }
            CaptureStageConfig capture_cfg = node.config.capture;
            if (capture_cfg.stream_id.empty()) {
                capture_cfg.stream_id = config_.stream_id;
            }
            std::unique_ptr<IVideoSource> source =std::make_unique<V4L2VideoSource>(capture_cfg.source);
            node.stage = std::make_unique<CaptureStage>(capture_cfg, std::move(source), *out_q);
            RKCAM_LOGI("[%s] create CaptureStage: %s -> %s",
                    config_.stream_id.c_str(),
                    node.config.name.c_str(),
                    node.config.output_queue.name.c_str());

            return true;    
        }
        case StageType::Rga:{
            auto* in_q = getTypeQueue<PipelineVideoFrame, PipelineQueueValueType::PipelineVideoFrame>(
                node.input_queue,
                node.config.name
            );
            auto* out_q = getTypeQueue<PipelineVideoFrame, PipelineQueueValueType::PipelineVideoFrame>(
                node.output_queue,
                node.config.name
            );
            if (!in_q || !out_q) {
                RKCAM_LOGE("[%s] RgaStage %s requires VideoFrame input/output queues",
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
                *in_q,
                *out_q);
            RKCAM_LOGI("[%s] create RgaStage: %s %s -> %s",
                    config_.stream_id.c_str(),
                    node.config.name.c_str(),
                    node.config.input_queue.name.c_str(),
                    node.config.output_queue.name.c_str());

            return true;
        }
        case StageType::Fps: {
            auto* in_q = getTypeQueue<
                PipelineVideoFrame,
                PipelineQueueValueType::PipelineVideoFrame>(
                    node.input_queue,
                    node.config.name);

            if (!in_q) {
                RKCAM_LOGE("[%s] FpsStage %s requires VideoFrame input_queue",
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
                *in_q);

            RKCAM_LOGI("[%s] create FpsStage: %s <- %s",
                    config_.stream_id.c_str(),
                    node.config.name.c_str(),
                    node.config.input_queue.name.c_str());
            return true;
        }
        case StageType::RawSave: {
            auto* in_q = getTypeQueue<
                PipelineVideoFrame,
                PipelineQueueValueType::PipelineVideoFrame>(
                    node.input_queue,
                    node.config.name);

            if (!in_q) {
                RKCAM_LOGE("[%s] RawSaveStage %s requires VideoFrame input_queue",
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
                *in_q);

            RKCAM_LOGI("[%s] create RawSaveStage: %s <- %s",
                    config_.stream_id.c_str(),
                    node.config.name.c_str(),
                    node.config.input_queue.name.c_str());

            return true;
        }
        case StageType::Mpp:{
            auto* in_q = getTypeQueue<PipelineVideoFrame, PipelineQueueValueType::PipelineVideoFrame>(node.input_queue, node.config.name);
            auto* out_q = getTypeQueue<EncodedPacket, PipelineQueueValueType::EncodedPacket>(node.output_queue, node.config.name);
            if (!in_q || !out_q) {
                RKCAM_LOGE("[%s] MppStage %s requires PipelineVideoFrame input and EncodedPacket output",
                        config_.stream_id.c_str(),
                        node.config.name.c_str());
                return false;
            }
            MppStageConfig mpp_cfg = node.config.mpp;
            if(mpp_cfg.stage_name.empty())
            {
                mpp_cfg.stage_name = node.config.name;
            }
            node.stage = std::make_unique<MppStage>(mpp_cfg, *in_q, *out_q);
                RKCAM_LOGI("[%s] create MppStage: %s %s -> %s",
               config_.stream_id.c_str(),
               node.config.name.c_str(),
               node.config.input_queue.name.c_str(),
               node.config.output_queue.name.c_str());

            return true;

        }
        case StageType::EncodedSave: {
            auto* in_q = getTypeQueue<
                EncodedPacket,
                PipelineQueueValueType::EncodedPacket>(
                    node.input_queue,
                    node.config.name);

            if (!in_q) {
                RKCAM_LOGE("[%s] EncodedSaveStage %s requires EncodedPacket input_queue",
                        config_.stream_id.c_str(),
                        node.config.name.c_str());
                return false;
            }

            EncodedSaveStageConfig save_cfg = node.config.encoded_save;

            if (save_cfg.stage_name.empty()) {
                save_cfg.stage_name = node.config.name;
            }

            node.stage = std::make_unique<EncodedSaveStage>(
                save_cfg,
                *in_q);

            RKCAM_LOGI("[%s] create EncodedSaveStage: %s <- %s",
                    config_.stream_id.c_str(),
                    node.config.name.c_str(),
                    node.config.input_queue.name.c_str());

            return true;
        }
        case StageType::Mp4Record:{
            auto * in_q = getTypeQueue<EncodedPacket, PipelineQueueValueType::EncodedPacket>(node.input_queue, node.config.name.c_str());
            if (!in_q) {
                RKCAM_LOGE("[%s] Mp4RecordStage %s requires EncodedPacket input_queue",
                        config_.stream_id.c_str(),
                        node.config.name.c_str());
                return false;
            }

            Mp4RecordStageConfig mp4_cfg = node.config.mp4_record;
            if (mp4_cfg.stage_name.empty()) {
                mp4_cfg.stage_name = node.config.name;
            }

            node.stage = std::make_unique<Mp4RecordStage>(mp4_cfg, *in_q);

            RKCAM_LOGI("[%s] create Mp4RecordStage: %s <- %s",
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
    for (auto& node : nodes_) {
        node.stage.reset();
        node.input_queue.reset();
        node.output_queue.reset();
    }

    nodes_.clear();
    queue_map_.clear();
}



} // namespace rkcam