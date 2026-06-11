#pragma once

namespace rkcam
{
class IStage
{
public:
    virtual ~Istage() = default;
    virtual bool start() = 0;
    virtual void stop() = 0;
};






}