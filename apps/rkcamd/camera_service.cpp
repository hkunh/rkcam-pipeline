#include "camera_service.hpp"

#include "rkcam/core/log.hpp"

namespace rkcam {

CameraService::CameraService(
    const CameraPipelineConfig& config)
    : pipeline_(config)
{
}

CameraService::~CameraService()
{
    stop();
}

bool CameraService::start()
{
    if (running_) {
        return true;
    }

    if (!pipeline_.start()) {
        RKCAM_LOGE(
            "[camera_service] pipeline start failed");

        return false;
    }

    running_ = true;

    RKCAM_LOGI(
        "[camera_service] started");

    return true;
}

void CameraService::stop()
{
    if (!running_) {
        return;
    }

    /*
     * CameraPipeline::stop()本身必须负责
     * 正确结束正在进行的录像会话。
     */
    pipeline_.stop();

    running_ = false;

    RKCAM_LOGI(
        "[camera_service] stopped");
}

bool CameraService::isRunning() const
{
    return running_;
}

bool CameraService::startRecording(
    const std::string& output_path)
{
    if (!running_) {
        RKCAM_LOGE(
            "[camera_service] camera is not running");

        return false;
    }

    /*
     * 当前业务层还没有额外策略，
     * 后续存储管理、状态管理等在这里加入。
     */
    return pipeline_.startRecording(
        output_path);
}

bool CameraService::stopRecording()
{
    if (!running_) {
        return false;
    }

    return pipeline_.stopRecording();
}

RecordingState
CameraService::recordingState() const
{
    if (!running_) {
        return RecordingState::Idle;
    }

    return pipeline_.recordingState();
}

} // namespace rkcam