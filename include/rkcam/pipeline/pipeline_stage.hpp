#pragma once

namespace rkcam
{
class IStage
{
public:
    virtual ~IStage() = default;
    virtual bool start() = 0;
    virtual void stop() = 0;
};

}