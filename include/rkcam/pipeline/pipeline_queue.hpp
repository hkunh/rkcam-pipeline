#pragma once

#include "rkcam/core/blocking_queue.hpp"
#include "rkcam/video/video_frame.hpp"
#include "rkcam/media/encoded_packet.hpp"

#include <cstddef>
#include <memory>
#include <string>

namespace rkcam {

enum class PipelineQueueValueType{
    Unknown,
    PipelineVideoFrame,
    EncodedPacket,
};

struct PipelineQueueConfig{

    std::string name;

    PipelineQueueValueType value_type = PipelineQueueValueType::PipelineVideoFrame;

    size_t capacity = 4;
    QueueFullPolicy policy = QueueFullPolicy::DropOldest;


    bool valid() const{
        
        return !name.empty();
    }
};
class IPipelineQueue{
public:
    virtual ~IPipelineQueue() = default;

    virtual PipelineQueueValueType valueType() const = 0;

    virtual void stop() = 0;
    virtual void clear() = 0;
    virtual void reset() = 0;

    virtual size_t size() const = 0;
    virtual bool stopped() const = 0;
};

template <typename T, PipelineQueueValueType TypeValue>
class PipelineQueueBox : public IPipelineQueue{
public:
    PipelineQueueBox(size_t capacity, QueueFullPolicy policy) : queue_(capacity, policy)
    {

    }
    
    PipelineQueueValueType valueType() const override{
        return TypeValue;
    }

    BlockingQueue<T>& queue()
    {
        return queue_;
    }

    const BlockingQueue<T>& queue() const
    {
        return queue_;
    }
    
    void stop() override
    {
        queue_.stop();
    }

    void clear() override
    {
        queue_.clear();
    }

    void reset() override
    {
        queue_.reset();
    }

    size_t size() const override
    {
        return queue_.size();
    }

    bool stopped() const override
    {
        return queue_.stopped();
    }
private:
    BlockingQueue<T> queue_;    

};
using VideoFrameQueueBox = PipelineQueueBox<PipelineVideoFrame, PipelineQueueValueType::PipelineVideoFrame>;

using EncodedPacketQueueBox = PipelineQueueBox<EncodedPacket, PipelineQueueValueType::EncodedPacket>;

}