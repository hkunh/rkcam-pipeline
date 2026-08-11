#pragma once

#include "rkcam/pipeline/camera_pipeline.hpp"
#include "rkcam/pipeline/mp4_record_stage.hpp"

#include <atomic>
#include <mutex>
#include <string>

namespace rkcam {

/*
 * CameraService
 *
 * 当前职责：
 *   1. 持有并管理 CameraPipeline 生命周期；
 *   2. 向上提供相机业务接口；
 *   3. 串行化 start/stop recording 等控制操作。
 *
 * 当前不负责：
 *   - IPC
 *   - Unix Socket
 *   - MPP具体控制
 *   - MP4 mux细节
 *   - RGA/DRM具体实现
 *
 * 后续 rkcamd 的 IPC 层直接调用 CameraService 即可。
 */
class CameraService {
public:
    explicit CameraService(
        const CameraPipelineConfig& config);

    ~CameraService();

    bool start();
    void stop();

    bool isRunning() const;

    bool startRecording(
        const std::string& output_path);

    bool stopRecording();

    RecordingState recordingState() const;

    bool startStreaming(const std::string& url);
    bool stopStreaming();
    StreamingState streamingState() const;

private:
    CameraPipeline pipeline_;

    std::atomic<bool> running_{false};
};
}