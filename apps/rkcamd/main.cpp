/**
*@file main.cpp
*@brief 
*@author HKH368
*@date 2026-05-29
*@return 
*/

#include "rkcam/core/log.hpp"
#include "rkcam/video/v4l2_video_source.hpp"


#include <chrono>
#include <csignal>
#include <cstdlib>
#include <string>

bool g_running = true;
static void signalHandler(int)
{
    g_running = false;
}


int main()
{
    RKCAM_LOGI("rkcamd start");
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    rkcam::V4L2VideoSourceConfig config;
    config.device = "/dev/video0";
    config.width = 1920;
    config.height = 1080;
    config.pixel_format = "NV12";
    config.buffer_count = 4;

    rkcam::V4L2VideoSource source(config);
        if (!source.open()) {
        RKCAM_LOGE("source.open failed");
        return 1;
    }

    if (!source.start()) {
        RKCAM_LOGE("source.start failed");
        source.close();
        return 1;
    }

    RKCAM_LOGI("rkcamd capture loop start");
    int64_t frame_count = 0;
    auto t0 = std::chrono::steady_clock::now();
    auto last = t0;
    while(g_running)
    {
        rkcam::VideoFrame frame;
        if(!source.readFrame(frame)){
            continue;
        }
        frame_count ++;
        if(!source.releaseFrame(frame))
        {
            RKCAM_LOGE("releaseFrame failed");
            break;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - last).count();
        if(elapsed >= 1.0)
        {
            auto total_elapsed = std::chrono::duration<double>(now - t0).count();
            double avg_fps = total_elapsed > 0 ? (static_cast<double>(frame_count) / total_elapsed) : 0.0;
            RKCAM_LOGI("frames=%lld, avg_fps=%.2f",
                       static_cast<long long>(frame_count),
                       avg_fps);
            last = now;
        }


    }
    
    RKCAM_LOGI("rkcamd stopping");

    source.stop();
    source.close();

    RKCAM_LOGI("rkcamd exit, total frames=%lld",
               static_cast<long long>(frame_count));
    
    return 0;
}
