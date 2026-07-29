#pragma once

#include "rkcam/audio/audio_frame.hpp"

namespace rkcam {

class IAudioSource{
public:
    virtual ~IAudioSource() = default;

    virtual bool open() = 0;
    virtual bool start() = 0;

    /*
     * 阻塞读取一帧音频。
     *
     * 第一版：
     *   一帧 audio frame = ALSA 一个 period。
     */
    virtual bool readFrame(AudioSourceFrame& frame) = 0;
    /*
     * ALSA readi 模式下暂时不需要显式释放。
     * 保留接口是为了和 VideoSource 风格统一。
     */
    virtual void releaseFrame(AudioSourceFrame& frame) = 0;


    virtual void stop() = 0;
    virtual void close() = 0;
};



}