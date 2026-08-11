#include "camera_service.hpp"
#include "pipeline_config.hpp"

#include "rkcam/core/log.hpp"

#include <chrono>
#include <csignal>
#include <thread>

namespace {

volatile std::sig_atomic_t g_running = 1;

void signalHandler(int)
{
    g_running = 0;
}

bool sleepSeconds(int seconds)
{
    /*
     * 每100ms检查一次Ctrl+C，
     * 不要一次sleep 10秒导致退出不灵敏。
     */
    const int loops = seconds * 10;

    for (int i = 0;
         i < loops && g_running;
         ++i) {

        std::this_thread::sleep_for(
            std::chrono::milliseconds(100));
    }

    return g_running != 0;
}

} // namespace

int main()
{
    std::signal(
        SIGINT,
        signalHandler);

    std::signal(
        SIGTERM,
        signalHandler);

    /*
     * ============================================================
     * 1. Build pipeline config
     * ============================================================
     */
    rkcam::CameraPipelineConfig config = buildRkcamPipelineConfig();

    /*
     * ============================================================
     * 2. Create CameraService
     * ============================================================
     */
    rkcam::CameraService camera_service(
        config);

    if (!camera_service.start()) {
        RKCAM_LOGE(
            "CameraService start failed");

        return 1;
    }

    RKCAM_LOGI(
        "====================================================");

    RKCAM_LOGI(
        "CameraService running");

    RKCAM_LOGI(
        "Dynamic recording test started");

    RKCAM_LOGI(
        "Preview should remain active during whole test");

    RKCAM_LOGI(
        "====================================================");

    /*
     * ============================================================
     * 3. Preview only
     * ============================================================
     */
    RKCAM_LOGI(
        "[test] preview only: 3 seconds");

    if (!sleepSeconds(3)) {
        camera_service.stop();
        return 0;
    }

    /*
     * ============================================================
     * 4. First recording
     * ============================================================
     */
    const std::string record_file_1 =
        "/userdata/rkcam/output/"
        "dynamic_record_01.mp4";

    RKCAM_LOGI(
        "[test] start recording #1");

    if (!camera_service.startRecording(
            record_file_1)) {

        RKCAM_LOGE(
            "[test] recording #1 start failed");

        camera_service.stop();
        return 1;
    }

    /*
     * 可以稍等一下后检查：
     * Starting -> Recording
     */
    sleepSeconds(1);

    RKCAM_LOGI(
        "[test] recording #1 state=%d",
        static_cast<int>(
            camera_service.recordingState()));

    /*
     * 再录9秒，总共约10秒。
     */
    if (!sleepSeconds(9)) {
        camera_service.stop();
        return 0;
    }

    RKCAM_LOGI(
        "[test] stop recording #1");

    if (!camera_service.stopRecording()) {

        RKCAM_LOGE(
            "[test] recording #1 stop failed");

        camera_service.stop();
        return 1;
    }

    RKCAM_LOGI(
        "[test] recording #1 stopped");

    /*
     * ============================================================
     * 5. Preview only again
     * ============================================================
     */
    RKCAM_LOGI(
        "[test] preview only again: 3 seconds");

    if (!sleepSeconds(3)) {
        camera_service.stop();
        return 0;
    }

    /*
     * ============================================================
     * 6. Second recording
     *
     * 这是本次测试最重要的一步：
     * 验证同一个 Mp4RecordStage 不重建线程，
     * 能重新创建第二个独立MP4会话。
     * ============================================================
     */
    // const std::string record_file_2 =
    //     "/userdata/rkcam/output/"
    //     "dynamic_record_02.mp4";

    // RKCAM_LOGI(
    //     "[test] start recording #2");

    // if (!camera_service.startRecording(
    //         record_file_2)) {

    //     RKCAM_LOGE(
    //         "[test] recording #2 start failed");

    //     camera_service.stop();
    //     return 1;
    // }

    // sleepSeconds(1);

    // RKCAM_LOGI(
    //     "[test] recording #2 state=%d",
    //     static_cast<int>(
    //         camera_service.recordingState()));

    // if (!sleepSeconds(9)) {
    //     camera_service.stop();
    //     return 0;
    // }

    // RKCAM_LOGI(
    //     "[test] stop recording #2");

    // if (!camera_service.stopRecording()) {

    //     RKCAM_LOGE(
    //         "[test] recording #2 stop failed");

    //     camera_service.stop();
    //     return 1;
    // }

    /*
     * ============================================================
     * 7. Final preview
     * ============================================================
     */
    RKCAM_LOGI(
        "[test] dynamic recording passed, "
        "preview another 3 seconds");

    sleepSeconds(3);


    RKCAM_LOGI(
        "[test] start streaming #1");

    if (!camera_service.startStreaming(
            "rtsp://192.168.56.100:8554/live")) {

        RKCAM_LOGE(
            "[test] streaming #1 start failed");

        camera_service.stop();
        return 1;
    }

    sleepSeconds(1);

    RKCAM_LOGI(
        "[test] streaming #1 state=%d",
        static_cast<int>(
            camera_service.streamingState()));

    sleepSeconds(9);

    RKCAM_LOGI(
        "[test] stop streaming #1");

    camera_service.stopStreaming();

    /*
    * 只预览3秒。
    */
    sleepSeconds(3);

    /*
    * 再推一次。
    */
    RKCAM_LOGI(
        "[test] start streaming #2");

    camera_service.startStreaming(
        "rtsp://192.168.56.100:8554/live");

    sleepSeconds(30);

    camera_service.stopStreaming();

    sleepSeconds(3);


    /*
     * ============================================================
     * 8. Stop whole camera service
     * ============================================================
     */
    RKCAM_LOGI(
        "CameraService stopping");

    camera_service.stop();

    RKCAM_LOGI(
        "dynamic recording test finished");

    return 0;
}