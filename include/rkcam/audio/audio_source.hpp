/**
*@file include/rkcam/audio/audio_source.hpp
*@brief 
*@author HKH368
*@date 2026-05-29
*@return 
*/

#pragma once

#include "rkcam/audio/audio_frame.hpp"

namespace rkcam {

class IAudioSource {
public:
    virtual ~IAudioSource() = default;

    virtual bool open() = 0;
    virtual bool start() = 0;
    virtual bool readFrame(AudioFrame& frame) = 0;
    virtual bool stop() = 0;
    virtual bool close() = 0;
};

}
