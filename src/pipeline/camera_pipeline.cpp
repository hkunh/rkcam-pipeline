#include "rkcam/pipeline/camera_pipeline.hpp"
#include "rkcam/core/log.hpp"
#include "rkcam/video/v4l2_video_source.hpp"

#include <memory>
#include <utility>

namespace rkcam {

namespace{
static int64_t nowMonotonicUs()
{
    timespec ts{};
    if(clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
        return -1;
    }
    return static_cast<int64_t>(ts.tv_sec) * 1000000LL +
        static_cast<int64_t>(ts.tv_nsec) / 1000LL;
}

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
    return &typed->queue();  //返回队列的地址
}
std::shared_ptr<IPipelineQueue> getSingleOutputQueue(
    const StageNode& node,
    const std::string& stream_id)
{
    if (node.output_queues.size() != 1) {
        RKCAM_LOGE("[%s] stage %s requires exactly one output queue, got=%zu",
                   stream_id.c_str(),
                   node.config.name.c_str(),
                   node.output_queues.size());
        return nullptr;
    }

    return node.output_queues[0];
}
std::shared_ptr<IPipelineQueue> getSingleInputQueue(
    const StageNode& node,
    const std::string& stream_id)
{
    if (node.input_queues.size() != 1) {
        RKCAM_LOGE(
            "[%s] stage %s requires exactly one input queue, got=%zu",
            stream_id.c_str(),
            node.config.name.c_str(),
            node.input_queues.size());

        return nullptr;
    }

    return node.input_queues[0];
}

}//namespace



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
    /*
     * 初始业务状态：
     *
     * Pipeline启动后先只有Preview。
     */
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        recording_requested_ = false;
        streaming_requested_ = false;

        if(video_frame_tee_stage_ && !config_.video_encode_output_queue_name.empty())
        {
            if(!video_frame_tee_stage_->setOutputEnabled(config_.video_encode_output_queue_name, false))
            {
                destroy();
                return false;
            }
        }
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
    mpp_stage_ = nullptr;
    mp4_record_stage_ = nullptr;
    rtsp_push_stage_ = nullptr;
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
         * 初始化所有输入队列。
         */
        node.input_queues.clear();
        for (const auto& queue_cfg : node.config.input_queues) {
            if (!queue_cfg.valid()) {
                RKCAM_LOGE(
                    "[%s] node %s has invalid input queue config",
                    config_.stream_id.c_str(),
                    node.config.name.c_str());
                return false;
            }

            std::shared_ptr<IPipelineQueue> queue;

            if (!createOrReuseQueue(queue_cfg, queue)) {
                RKCAM_LOGE(
                    "[%s] create input queue failed: "
                    "stage=%s queue=%s",
                    config_.stream_id.c_str(),
                    node.config.name.c_str(),
                    queue_cfg.name.c_str());
                return false;
            }
            node.input_queues.push_back(std::move(queue));
        }

        node.output_queues.clear();
        /*
        * 初始化 output queue。
        * RawSave / Fps 这类 sink stage 可以没有 output queue。
        */
        for(const auto& queue_cfg : node.config.output_queues)
        {
            if (!queue_cfg.valid()) {
                RKCAM_LOGE("[%s] node %s has invalid output queue config",
                           config_.stream_id.c_str(),
                           node.config.name.c_str());
                return false;
            }
            std::shared_ptr<IPipelineQueue> queue;
            if (!createOrReuseQueue(queue_cfg, queue)) {
                return false;
            }

            node.output_queues.push_back(queue);
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
        case PipelineQueueValueType::PipelineAudioFrame:
            new_queue = std::make_shared<AudioFrameQueueBox>(config.capacity, config.policy);
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
            auto output_box = getSingleOutputQueue(node, config_.stream_id);
            if (!output_box) {
                return false;
            }
            auto* out_q = getTypeQueue<PipelineVideoFrame, PipelineQueueValueType::PipelineVideoFrame>(
                output_box,
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
                    node.config.output_queues[0].name.c_str());

            return true;    
        }
        case StageType::Rga:{
            auto input_box = getSingleInputQueue(node, config_.stream_id);
            auto output_box = getSingleOutputQueue(node, config_.stream_id);
            if (!input_box || !output_box) {
                return false;
            }
            auto* in_q = getTypeQueue<PipelineVideoFrame, PipelineQueueValueType::PipelineVideoFrame>(
                input_box,
                node.config.name
            );
            auto* out_q = getTypeQueue<PipelineVideoFrame, PipelineQueueValueType::PipelineVideoFrame>(
                output_box,
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
                    node.config.input_queues[0].name.c_str(),
                    node.config.output_queues[0].name.c_str());

            return true;
        }
        case StageType::Fps: {
            auto input_box = getSingleInputQueue(node, config_.stream_id);
            auto* in_q = getTypeQueue<
                PipelineVideoFrame,
                PipelineQueueValueType::PipelineVideoFrame>(
                    input_box,
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
                    node.config.input_queues[0].name.c_str());
            return true;
        }
        case StageType::RawSave: {
            auto input_box = getSingleInputQueue(node, config_.stream_id);
            auto* in_q = getTypeQueue<
                PipelineVideoFrame,
                PipelineQueueValueType::PipelineVideoFrame>(
                    input_box,
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
                    node.config.input_queues[0].name.c_str());

            return true;
        }
        case StageType::Mpp:{
            auto input_box = getSingleInputQueue(node, config_.stream_id);
            auto output_box = getSingleOutputQueue(node, config_.stream_id);
            if (!input_box || !output_box) {
                return false;
            }
            auto* in_q = getTypeQueue<PipelineVideoFrame, PipelineQueueValueType::PipelineVideoFrame>(input_box, node.config.name);
            auto* out_q = getTypeQueue<EncodedPacket, PipelineQueueValueType::EncodedPacket>(output_box, node.config.name);
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
            mpp_stage_ = dynamic_cast<rkcam::MppStage*>(node.stage.get());
            RKCAM_LOGI("[%s] create MppStage: %s %s -> %s",
               config_.stream_id.c_str(),
               node.config.name.c_str(),
               node.config.input_queues[0].name.c_str(),
               node.config.output_queues[0].name.c_str());

            return true;

        }
        case StageType::EncodedSave: {
            auto input_box = getSingleInputQueue(node, config_.stream_id);
            auto* in_q = getTypeQueue<
                EncodedPacket,
                PipelineQueueValueType::EncodedPacket>(
                    input_box,
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
                    node.config.input_queues[0].name.c_str());

            return true;
        }
        case StageType::Mp4Record:{
            if (!node.output_queues.empty()) {
                RKCAM_LOGE(
                    "[%s] Mp4RecordStage %s should not have output queues, got=%zu",
                    config_.stream_id.c_str(),
                    node.config.name.c_str(),
                    node.output_queues.size());
                return false;
            }

            Mp4RecordStageConfig mp4_cfg =
                node.config.mp4_record;

            if (mp4_cfg.stage_name.empty()) {
                mp4_cfg.stage_name =
                    node.config.name;
            }

            const size_t expected_input_count =
                static_cast<size_t>(mp4_cfg.video.enabled ? 1 : 0) +
                static_cast<size_t>(mp4_cfg.audio.enabled ? 1 : 0);

            if (node.input_queues.size() != expected_input_count) {
                RKCAM_LOGE(
                    "[%s] Mp4RecordStage %s requires %zu input queues, got=%zu",
                    config_.stream_id.c_str(),
                    node.config.name.c_str(),
                    expected_input_count,
                    node.input_queues.size());
                return false;
            }
            std::vector<EncodedPacketInputPort> ports;
            ports.reserve(expected_input_count);

            size_t input_index = 0;
            /*
            * 约定：
            *   视频输入在前，音频输入在后。
            */
            if(mp4_cfg.video.enabled)
            {
                auto* video_q = getTypeQueue<EncodedPacket, PipelineQueueValueType::EncodedPacket>(node.input_queues[input_index], node.config.name);
                if(!video_q)
                {
                    RKCAM_LOGE(
                        "[%s] Mp4RecordStage %s video input "
                        "requires EncodedPacket queue",
                        config_.stream_id.c_str(),
                        node.config.name.c_str());
                    return false;
                }
                EncodedPacketInputPort video_port;
                video_port.port_name = "video";
                video_port.stream_id = mp4_cfg.video.stream_id;
                video_port.media_type = MediaType::Video;
                video_port.codec = mp4_cfg.video.codec;
                video_port.queue = video_q;
                video_port.required = true;
                ports.push_back(std::move(video_port));
                ++input_index;
            }
            if(mp4_cfg.audio.enabled)
            {
                auto* audio_q = getTypeQueue<EncodedPacket, PipelineQueueValueType::EncodedPacket>(node.input_queues[input_index], node.config.name);
                if (!audio_q) {
                    RKCAM_LOGE(
                        "[%s] Mp4RecordStage %s audio input "
                        "requires EncodedPacket queue",
                        config_.stream_id.c_str(),
                        node.config.name.c_str());
                    return false;
                }
                EncodedPacketInputPort audio_port;
                audio_port.port_name = "audio";
                audio_port.stream_id = mp4_cfg.audio.stream_id;
                audio_port.media_type = MediaType::Audio;
                audio_port.codec = mp4_cfg.audio.codec;
                audio_port.queue = audio_q;
                audio_port.required = true;

                ports.push_back(std::move(audio_port));
                ++input_index;
            }
            node.stage = std::make_unique<Mp4RecordStage>(mp4_cfg, ports);
            mp4_record_stage_ = static_cast<rkcam::Mp4RecordStage*>(node.stage.get());
            RKCAM_LOGI(
                "[%s] create Mp4RecordStage: %s "
                "inputs=%zu video=%d audio=%d output=%s",
                config_.stream_id.c_str(),
                node.config.name.c_str(),
                ports.size(),
                mp4_cfg.video.enabled ? 1 : 0,
                mp4_cfg.audio.enabled ? 1 : 0,
                mp4_cfg.output_path.c_str());

            return true;
        }
        case StageType::RtspPush:{
            if (!node.output_queues.empty()) {
                RKCAM_LOGE(
                    "[%s] RtspPushStage %s should not have output queues, got=%zu",
                    config_.stream_id.c_str(),
                    node.config.name.c_str(),
                    node.output_queues.size());
                return false;
            }
            RtspPushStageConfig rtsp_cfg = node.config.rtsp_push;
            if (rtsp_cfg.stage_name.empty()) {
                rtsp_cfg.stage_name =
                    node.config.name;
            }
            /*
            * 当前 RtspPushStage 仍然要求视频开启，
            * 并以第一帧视频 IDR 作为推流时间轴起点。
            */
            if(!rtsp_cfg.video.enabled)
            {
                RKCAM_LOGE(
                    "[%s] RtspPushStage %s currently requires video enabled",
                    config_.stream_id.c_str(),
                    node.config.name.c_str());
                return false;
            }
            /*
            * 视频 stream_id 默认继承 CameraPipeline 的 stream_id。
            */
            if(rtsp_cfg.video.stream_id.empty())
            {
                rtsp_cfg.video.stream_id = config_.stream_id;
            }
            /*
            * 音频 stream_id 必须与 AacEncodeStage 输出保持一致。
            */
            if (rtsp_cfg.audio.enabled &&
                rtsp_cfg.audio.stream_id.empty()) {
                rtsp_cfg.audio.stream_id = "audio0";
            }
            const size_t expected_input_count = static_cast<size_t>(rtsp_cfg.video.enabled ? 1 : 0) +
                                                static_cast<size_t>(rtsp_cfg.audio.enabled ? 1 : 0);
            if(node.input_queues.size() != expected_input_count){
                RKCAM_LOGE(
                    "[%s] RtspPushStage %s requires %zu input queues, got=%zu",
                    config_.stream_id.c_str(),
                    node.config.name.c_str(),
                    expected_input_count,
                    node.input_queues.size());
                return false;
            }
            std::vector<EncodedPacketInputPort> ports;
            ports.reserve(expected_input_count);
            size_t input_index = 0;
            /*
            * 固定约定：
            *
            * input_queues[0] = H264 video
            * input_queues[1] = AAC audio（启用音频时）
            */
            if(rtsp_cfg.video.enabled)
            {
                auto* video_q = getTypeQueue<EncodedPacket, PipelineQueueValueType::EncodedPacket>(node.input_queues[input_index], node.config.name);
                if (!video_q) {
                    RKCAM_LOGE(
                        "[%s] RtspPushStage %s video input "
                        "requires EncodedPacket queue",
                        config_.stream_id.c_str(),
                        node.config.name.c_str());
                    return false;
                }
                EncodedPacketInputPort video_port;
                video_port.port_name = "video";
                video_port.stream_id = rtsp_cfg.video.stream_id;
                video_port.media_type = MediaType::Video;
                video_port.codec = rtsp_cfg.video.codec;
                video_port.queue = video_q;
                video_port.required = true;
                ports.push_back(std::move(video_port));
                ++input_index;
            }
            if(rtsp_cfg.audio.enabled)
            {
                auto* audio_q = getTypeQueue<EncodedPacket, PipelineQueueValueType::EncodedPacket>(node.input_queues[input_index], node.config.name);
                if (!audio_q) {
                    RKCAM_LOGE(
                        "[%s] RtspPushStage %s audio input "
                        "requires EncodedPacket queue",
                        config_.stream_id.c_str(),
                        node.config.name.c_str());
                    return false;
                }
                EncodedPacketInputPort audio_port;
                audio_port.port_name = "audio";
                audio_port.stream_id = rtsp_cfg.audio.stream_id;
                audio_port.media_type = MediaType::Audio;
                audio_port.queue = audio_q;
                audio_port.required = true;
                ports.push_back(std::move(audio_port));
                ++input_index;
            }
            node.stage = std::make_unique<RtspPushStage>(rtsp_cfg, ports);
            rtsp_push_stage_ = static_cast<rkcam::RtspPushStage*>(node.stage.get());
            RKCAM_LOGI(
                "[%s] create RtspPushStage: %s "
                "inputs=%zu video=%s audio=%s audio_enabled=%d url=%s",
                config_.stream_id.c_str(),
                node.config.name.c_str(),
                ports.size(),
                rtsp_cfg.video.stream_id.c_str(),
                rtsp_cfg.audio.stream_id.c_str(),
                rtsp_cfg.audio.enabled ? 1 : 0,
                rtsp_cfg.url.c_str());

            return true;

        }
        case StageType::Display:{
            auto input_box = getSingleInputQueue(node, config_.stream_id);
            auto* in_q = getTypeQueue<PipelineVideoFrame, PipelineQueueValueType::PipelineVideoFrame>(input_box, node.config.name);
            if (!in_q) {
                return false;
            }

            DisplayStageConfig display_cfg = node.config.display;
            if (display_cfg.stage_name.empty()) {
                display_cfg.stage_name = node.config.name;
            }
            auto sink = std::make_unique<DrmDisplaySink>(node.config.drm_display);
            node.stage = std::make_unique<DisplayStage>(display_cfg, *in_q, std::move(sink));

            RKCAM_LOGI("[%s] create DisplayStage: %s <- %s",
                    config_.stream_id.c_str(),
                    node.config.name.c_str(),
                    node.config.input_queues[0].name.c_str());

            return true;
        }
        case StageType::VideoFrameTee:{
            
            if (node.input_queues.empty()) {
                RKCAM_LOGE("[%s] VideoFrameTeeStage %s requires input_queues",
                        config_.stream_id.c_str(),
                        node.config.name.c_str());
                return false;
            }
            if (node.output_queues.empty()) {
                RKCAM_LOGE("[%s] VideoFrameTeeStage %s requires output_queues",
                        config_.stream_id.c_str(),
                        node.config.name.c_str());
                return false;
            }
            auto input_box = getSingleInputQueue(node, config_.stream_id);
            auto* in_q = getTypeQueue<PipelineVideoFrame, PipelineQueueValueType::PipelineVideoFrame>(input_box, node.config.name);
            if (!in_q) {
                return false;
            }

            // std::vector<BlockingQueue<PipelineVideoFrame>*> out_queues;
            // for(const auto& output_box : node.output_queues)
            // {
            //     auto* out_q = getTypeQueue<PipelineVideoFrame, PipelineQueueValueType::PipelineVideoFrame>(output_box, node.config.name);
            //     if (!out_q) {
            //         return false;
            //     }

            //     out_queues.push_back(out_q);
            // }

            std::vector<VideoFrameTeeOutputPort> ports;
            ports.reserve(node.output_queues.size());

            for(size_t i = 0; i < node.output_queues.size(); ++i)
            {
                auto* out_q = getTypeQueue<PipelineVideoFrame, PipelineQueueValueType::PipelineVideoFrame>(node.output_queues[i], node.config.name);
                if(!out_q)
                {
                    return false;
                }

                VideoFrameTeeOutputPort port;
                /*
                * 用PipelineQueueConfig.name作为稳定标识。
                */
                port.name = node.config.output_queues[i].name;
                port.queue = out_q;
                port.enabled = true;
                ports.push_back(std::move(port));
            }



            VideoFrameTeeStageConfig tee_cfg = node.config.video_frame_tee;
            if (tee_cfg.stage_name.empty()) {
                tee_cfg.stage_name = node.config.name;
            }

            node.stage = std::make_unique<VideoFrameTeeStage>(tee_cfg, *in_q, ports);
            video_frame_tee_stage_ = static_cast<rkcam::VideoFrameTeeStage*>(node.stage.get());
            RKCAM_LOGI("[%s] create VideoFrameTeeStage: %s outputs=%zu",
                    config_.stream_id.c_str(),
                    node.config.name.c_str(),
                    ports.size());

            return true;
        }
        case StageType::EncodedPacketTee: {
            if (node.input_queues.empty()) {
                RKCAM_LOGE("[%s] EncodedPacketTeeStage %s requires input_queues",
                        config_.stream_id.c_str(),
                        node.config.name.c_str());
                return false;
            }

            if (node.output_queues.empty()) {
                RKCAM_LOGE("[%s] EncodedPacketTeeStage %s requires output_queues",
                        config_.stream_id.c_str(),
                        node.config.name.c_str());
                return false;
            }
            auto input_box = getSingleInputQueue(node, config_.stream_id);
            auto* in_q = getTypeQueue<
                EncodedPacket,
                PipelineQueueValueType::EncodedPacket>(
                    input_box,
                    node.config.name);

            if (!in_q) {
                return false;
            }

            std::vector<BlockingQueue<EncodedPacket>*> out_queues;

            for (const auto& output_box : node.output_queues) {
                auto* out_q = getTypeQueue<
                    EncodedPacket,
                    PipelineQueueValueType::EncodedPacket>(
                        output_box,
                        node.config.name);

                if (!out_q) {
                    return false;
                }

                out_queues.push_back(out_q);
            }

            EncodedPacketTeeStageConfig tee_cfg = node.config.encoded_packet_tee;
            if (tee_cfg.stage_name.empty()) {
                tee_cfg.stage_name = node.config.name;
            }

            node.stage = std::make_unique<EncodedPacketTeeStage>(
                tee_cfg,
                *in_q,
                out_queues);

            RKCAM_LOGI("[%s] create EncodedPacketTeeStage: %s outputs=%zu",
                    config_.stream_id.c_str(),
                    node.config.name.c_str(),
                    out_queues.size());

            return true;
        }
        case StageType::AudioCapture:{
            if(node.output_queues.size() != 1)
            {
                RKCAM_LOGE("[%s] AudioCaptureStage %s requires exactly one output queue, got=%zu",
                        config_.stream_id.c_str(),
                        node.config.name.c_str(),
                        node.output_queues.size());
                return false;
            }

            auto* out_q = getTypeQueue<PipelineAudioFrame, PipelineQueueValueType::PipelineAudioFrame>(node.output_queues[0], node.config.name);
            if(!out_q)
            {
                return false;
            }

            AudioCaptureStageConfig audio_cfg = node.config.audio_capture;
            if (audio_cfg.stage_name.empty()) {
                audio_cfg.stage_name = node.config.name;
            }

            if (audio_cfg.stream_id.empty()) {
                audio_cfg.stream_id = "audio0";
            }

            /*
            * 保持 source.stream_id 和 stage stream_id 一致。
            */
            if (audio_cfg.source.stream_id.empty()) {
                audio_cfg.source.stream_id = audio_cfg.stream_id;
            }

            auto source = std::make_unique<AlsaAudioSource>(audio_cfg.source);
            node.stage = std::make_unique<AudioCaptureStage>(audio_cfg, std::move(source), *out_q);

            RKCAM_LOGI("[%s] create AudioCaptureStage: %s -> %s device=%s",
                    config_.stream_id.c_str(),
                    node.config.name.c_str(),
                    node.config.output_queues[0].name.c_str(),
                    audio_cfg.source.device.c_str());

            return true;

        }
        case StageType::WavSave: {
            if (node.input_queues.empty()) {
                RKCAM_LOGE("[%s] WavSaveStage %s requires input_queues",
                        config_.stream_id.c_str(),
                        node.config.name.c_str());
                return false;
            }

            if (!node.output_queues.empty()) {
                RKCAM_LOGE("[%s] WavSaveStage %s should not have output_queues, got=%zu",
                        config_.stream_id.c_str(),
                        node.config.name.c_str(),
                        node.output_queues.size());
                return false;
            }
            auto input_box = getSingleInputQueue(node, config_.stream_id);
            auto* in_q = getTypeQueue<PipelineAudioFrame, PipelineQueueValueType::PipelineAudioFrame>(
                input_box,
                node.config.name  
            );
            WavSaveStageConfig wav_cfg = node.config.wav_save;
            if (wav_cfg.stage_name.empty()) {
                wav_cfg.stage_name = node.config.name;
            }
            node.stage = std::make_unique<WavSaveStage>(wav_cfg, *in_q);

            RKCAM_LOGI("[%s] create WavSaveStage: %s <- %s output=%s",
                    config_.stream_id.c_str(),
                    node.config.name.c_str(),
                    node.config.input_queues[0].name.c_str(),
                    wav_cfg.output_path.c_str());

            return true;

        }
        case StageType::AacEncode:{
            if(node.input_queues.empty())
            {
                RKCAM_LOGE("[%s] AacEncodeStage %s requires input_queues",
                        config_.stream_id.c_str(),
                        node.config.name.c_str());
                return false;
            }
            if (node.output_queues.size() != 1) {
                RKCAM_LOGE("[%s] AacEncodeStage %s requires exactly one output queue, got=%zu",
                        config_.stream_id.c_str(),
                        node.config.name.c_str(),
                        node.output_queues.size());
                return false;
            }
            auto input_box = getSingleInputQueue(node, config_.stream_id);
            auto* in_q = getTypeQueue<PipelineAudioFrame, PipelineQueueValueType::PipelineAudioFrame>(
                input_box,
                node.config.name
            );
            if(!in_q){
                return false;
            }

            auto* out_q = getTypeQueue<EncodedPacket, PipelineQueueValueType::EncodedPacket>(
                node.output_queues[0],
                node.config.name
            );

            AacEncodeStageConfig aac_cfg = node.config.aac_encode;

            if(aac_cfg.stage_name.empty())
            {
                aac_cfg.stage_name = node.config.name;
            }

            if(aac_cfg.stream_id.empty())
            {
                aac_cfg.stream_id = "audio0";
            }

            if(aac_cfg.encoder.stream_id.empty())
            {
                aac_cfg.encoder.stream_id = aac_cfg.stream_id;
            }

            node.stage = std::make_unique<AacEncodeStage>(aac_cfg, *in_q, *out_q);

            RKCAM_LOGI("[%s] create AacEncodeStage: %s %s -> %s",
                    config_.stream_id.c_str(),
                    node.config.name.c_str(),
                    node.config.input_queues[0].name.c_str(),
                    node.config.output_queues[0].name.c_str());

            return true;
        }
        case StageType::AacAdtsSave: {
            if (node.input_queues.empty()) {
                RKCAM_LOGE("[%s] AacAdtsSaveStage %s requires input_queues",
                        config_.stream_id.c_str(),
                        node.config.name.c_str());
                return false;
            }

            if (!node.output_queues.empty()) {
                RKCAM_LOGE("[%s] AacAdtsSaveStage %s should not have output queues, got=%zu",
                        config_.stream_id.c_str(),
                        node.config.name.c_str(),
                        node.output_queues.size());
                return false;
            }
            auto input_box = getSingleInputQueue(node, config_.stream_id);
            auto* in_q = getTypeQueue<
                EncodedPacket,
                PipelineQueueValueType::EncodedPacket>(
                    input_box,
                    node.config.name);

            if (!in_q) {
                return false;
            }

            AacAdtsSaveStageConfig save_cfg = node.config.aac_adts_save;

            if (save_cfg.stage_name.empty()) {
                save_cfg.stage_name = node.config.name;
            }

            node.stage = std::make_unique<AacAdtsSaveStage>(
                save_cfg,
                *in_q);

            RKCAM_LOGI("[%s] create AacAdtsSaveStage: %s <- %s output=%s",
                    config_.stream_id.c_str(),
                    node.config.name.c_str(),
                    node.config.input_queues[0].name.c_str(),
                    save_cfg.output_path.c_str());

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
    /*
     * 这些都是指向node.stage对象的非拥有指针。
     * node.stage释放以后必须清空。
     */
    mpp_stage_ = nullptr;
    mp4_record_stage_ = nullptr;
    rtsp_push_stage_ = nullptr;

    config_.video_encode_output_queue_name.clear();

    recording_requested_ = false;
    streaming_requested_ = false;

    for (auto& node : nodes_) {
        node.stage.reset();
        node.input_queues.clear();

        node.output_queues.clear();
        
    }

    nodes_.clear();
    queue_map_.clear();
}
bool CameraPipeline::startRecording(const std::string& output_path)
{
    std::lock_guard<std::mutex> lock(control_mutex_);
    if(!running_ || !mp4_record_stage_ || !mpp_stage_)
    {
        return false;
    }
    const int64_t request_pts_us = nowMonotonicUs();
    /*
     * 1. 从当前 MPP encoder 获取最新 SPS/PPS。
     *
     * 如果以后编码参数变了，
     * 这里得到的也应该是新参数对应的 header。
     */
    std::vector<uint8_t> video_extradata;

    if (!mpp_stage_->getCodecHeader(
            video_extradata)) {

        RKCAM_LOGE(
            "[%s] startRecording: "
            "get current H264 header failed",
            config_.stream_id.c_str());

        return false;
    }

    /*
     * beginRecording() 会同步等待 Mux Writer
     * 真正进入 Starting。
     */
    if(!mp4_record_stage_->beginRecording(output_path, video_extradata, request_pts_us))
    {
        return false;
    }

    /*
     * 从现在开始，录像对video encode branch有需求。
     */
    recording_requested_ = true;


    /*
     * Recorder 已经准备好后再请求 IDR，
     * 避免 IDR 比 Start 命令先到。
     */
    if(!mpp_stage_->requestIdr(request_pts_us))
    {
        mp4_record_stage_->endRecording();
        return false;
    }

    /*
     * 最后才开始给RGA/MPP送新frame。
     *
     * 如果RTSP本来已经在推，
     * 当前Gate本来就是ON，这里不会重复操作。
     */
    if(!updateVideoEncodeBranchState())
    {
        recording_requested_ = false;
        mp4_record_stage_->endRecording();
        return false;
    }
    return true;

}
bool CameraPipeline::stopRecording()
{
    std::lock_guard<std::mutex> lock(control_mutex_);
    if(!running_ || !mp4_record_stage_)
    {
        return false;
    }
    if(!mp4_record_stage_->endRecording())
    {
        return false;
    }
    recording_requested_ = false;
    /*
     * 如果RTSP仍然Streaming：
     *   needed=true
     *   MPP继续工作
     *
     * 如果RTSP也关闭：
     *   needed=false
     *   Gate关闭
     */
    if(!updateVideoEncodeBranchState())
    {
        RKCAM_LOGE(
            "[%s] stopRecording: "
            "update encode gate failed",
            config_.stream_id.c_str());

        return false;
    }
    return true;
}
RecordingState CameraPipeline::recordingState() const
{
    if (!running_ || !mp4_record_stage_) {
        return RecordingState::Idle;
    }

    return mp4_record_stage_->recordingState();
}
bool CameraPipeline::startStreaming(const std::string& url)
{
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (!running_) {
        RKCAM_LOGE(
            "[%s] startStreaming failed: "
            "pipeline not running",
            config_.stream_id.c_str());

        return false;
    }

    if (!rtsp_push_stage_) {
        RKCAM_LOGE(
            "[%s] startStreaming failed: "
            "RtspPushStage unavailable",
            config_.stream_id.c_str());

        return false;
    }

    if (!mpp_stage_) {
        RKCAM_LOGE(
            "[%s] startStreaming failed: "
            "MppStage unavailable",
            config_.stream_id.c_str());

        return false;
    }
    if (url.empty()) {
        RKCAM_LOGE(
            "[%s] startStreaming failed: "
            "url empty",
            config_.stream_id.c_str());

        return false;
    }

    /*
     * 已经正在启动或推流，
     * 不允许重复创建第二个RTSP session。
     */    
     const StreamingState state = rtsp_push_stage_->streamingState();
    if (state != StreamingState::Idle &&
        state != StreamingState::Error) {

        RKCAM_LOGE(
            "[%s] startStreaming rejected: "
            "current state=%d",
            config_.stream_id.c_str(),
            static_cast<int>(state));

        return false;
    }
    

    /*
     * ============================================================
     * 1. 给RTSP和MPP使用同一个时间边界
     * ============================================================
     */
    const int64_t request_pts_us =
        nowMonotonicUs();

    if (request_pts_us < 0) {
        RKCAM_LOGE(
            "[%s] startStreaming failed: "
            "get monotonic time failed",
            config_.stream_id.c_str());

        return false;
    }

    /*
     * ============================================================
     * 2. 每次推流重新获取当前MPP H264 SPS/PPS
     * ============================================================
     */
    std::vector<uint8_t> video_extradata;
    if(!mpp_stage_->getCodecHeader(video_extradata))
    {
        RKCAM_LOGE(
            "[%s] startStreaming failed: "
            "get current H264 header failed",
            config_.stream_id.c_str());

        return false;
    }
    /*
     * ============================================================
     * 3. 先让RTSP进入Starting
     *
     * 这样新的IDR到达时，
     * RTSP Stage已经准备好接收。
     * ============================================================
     */
    if(!rtsp_push_stage_->beginStreaming(url, video_extradata, request_pts_us)){
        RKCAM_LOGE(
            "[%s] startStreaming failed: "
            "RtspPushStage beginStreaming failed",
            config_.stream_id.c_str());

        return false;
    }
    streaming_requested_ = true;
    /*
     * ============================================================
     * 4. 再请求第一帧PTS >= request_pts的帧成为IDR
     *
     * 和动态录像共用同一个MppStage请求机制。
     * ============================================================
     */
    if(!mpp_stage_->requestIdr(request_pts_us))
    {
        RKCAM_LOGE(
            "[%s] startStreaming failed: "
            "request IDR failed",
            config_.stream_id.c_str());

        /*
         * 回滚RTSP Session。
         */
        rtsp_push_stage_->
            endStreaming();

        return false;
    }
    /*
     * 最后打开编码数据源。
     */
    if (!updateVideoEncodeBranchState()) {

        streaming_requested_ = false;

        rtsp_push_stage_->endStreaming();

        return false;
    }


    RKCAM_LOGI(
        "[%s] streaming start requested: "
        "url=%s request_pts=%lld",
        config_.stream_id.c_str(),
        url.c_str(),
        static_cast<long long>(
            request_pts_us));

    return true;
}

bool CameraPipeline::stopStreaming()
{
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (!running_) {
        RKCAM_LOGE(
            "[%s] stopStreaming failed: "
            "pipeline not running",
            config_.stream_id.c_str());

        return false;
    }

    if (!rtsp_push_stage_) {
        RKCAM_LOGE(
            "[%s] stopStreaming failed: "
            "RtspPushStage unavailable",
            config_.stream_id.c_str());

        return false;
    }

    /*
     * endStreaming()只关闭当前RTSP Session：
     *
     *   av_write_trailer
     *   free AVFormatContext
     *   disconnect MediaMTX
     *
     * RtspPushStage的Reader/Mux线程继续运行。
     */
    if(!rtsp_push_stage_->endStreaming())
    {
        return false;
    }
    streaming_requested_ = false;
    return updateVideoEncodeBranchState();
}
StreamingState CameraPipeline::streamingState() const
{
    if (!running_ ||
        !rtsp_push_stage_) {

        return StreamingState::Idle;
    }
    return rtsp_push_stage_->streamingState();
}

bool CameraPipeline::updateVideoEncodeBranchState()
{
    if(!video_frame_tee_stage_ || config_.video_encode_output_queue_name.empty())
    {
        RKCAM_LOGE(
            "[%s] video encode gate unavailable",
            config_.stream_id.c_str());

        return false;
    }
    /*
     * 任意一个消费者需要视频编码，
     * 整个RGA→MPP分支就必须保持工作。
     */
    const bool needed = recording_requested_ || streaming_requested_;
    const bool current = video_frame_tee_stage_->outputEnabled(config_.video_encode_output_queue_name);
    if(needed == current)
    {
        return true;
    }
    if(!video_frame_tee_stage_->setOutputEnabled(config_.video_encode_output_queue_name, needed))
    {
        return false;
    }

    RKCAM_LOGI(
        "[%s] video encode branch: "
        "enabled=%d recording=%d streaming=%d",
        config_.stream_id.c_str(),
        needed ? 1 : 0,
        recording_requested_ ? 1 : 0,
        streaming_requested_ ? 1 : 0);

    return true;
}


} // namespace rkcam