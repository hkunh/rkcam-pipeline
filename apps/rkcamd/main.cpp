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
     * 1. Build pipeline
     * ============================================================
     */
    rkcam::CameraPipelineConfig config =
        buildRkcamPipelineConfig();

    rkcam::CameraService camera_service(
        config);

    if (!camera_service.start()) {
        RKCAM_LOGE(
            "[test] CameraService start failed");

        return 1;
    }

    const std::string rtsp_url =
        "rtsp://192.168.56.100:8554/live";

    const std::string record_file_1 =
        "/userdata/rkcam/output/"
        "gate_record_01.mp4";

    const std::string record_file_2 =
        "/userdata/rkcam/output/"
        "gate_record_02.mp4";

    RKCAM_LOGI(
        "====================================================");

    RKCAM_LOGI(
        "[test] Encode Gate integration test started");

    RKCAM_LOGI(
        "[test] Preview must remain active for whole test");

    RKCAM_LOGI(
        "====================================================");


    /*
     * ============================================================
     * 2. Preview only
     *
     * Expected:
     *   recording = false
     *   streaming = false
     *   encode gate = OFF
     *
     * Check logs:
     *   preview_rga/display counters keep increasing
     *   record_rga/mpp counters should NOT keep increasing
     * ============================================================
     */
    RKCAM_LOGI(
        "[test][1] PREVIEW ONLY: 5 seconds");

    if (!sleepSeconds(5)) {
        camera_service.stop();
        return 0;
    }


    /*
     * ============================================================
     * 3. Start recording
     *
     * Expected:
     *   record=1 stream=0
     *   encode gate OFF -> ON
     *   record_rga / MPP resume
     * ============================================================
     */
    RKCAM_LOGI(
        "[test][2] START RECORDING #1");

    if (!camera_service.startRecording(
            record_file_1)) {

        RKCAM_LOGE(
            "[test] start recording #1 failed");

        camera_service.stop();
        return 1;
    }

    if (!sleepSeconds(1)) {
        camera_service.stop();
        return 0;
    }

    RKCAM_LOGI(
        "[test] recording_state=%d",
        static_cast<int>(
            camera_service.recordingState()));

    if (!sleepSeconds(4)) {
        camera_service.stop();
        return 0;
    }


    /*
     * ============================================================
     * 4. Stop recording
     *
     * Expected:
     *   record=0 stream=0
     *   encode gate ON -> OFF
     *
     * record_rga / MPP may process a few in-flight frames,
     * then counters must stop.
     * ============================================================
     */
    RKCAM_LOGI(
        "[test][3] STOP RECORDING #1");

    if (!camera_service.stopRecording()) {
        RKCAM_LOGE(
            "[test] stop recording #1 failed");

        camera_service.stop();
        return 1;
    }

    RKCAM_LOGI(
        "[test] PREVIEW ONLY after recording: "
        "wait 5 seconds for branch drain");

    if (!sleepSeconds(5)) {
        camera_service.stop();
        return 0;
    }


    /*
     * ============================================================
     * 5. Start streaming
     *
     * Expected:
     *   record=0 stream=1
     *   encode gate OFF -> ON
     * ============================================================
     */
    RKCAM_LOGI(
        "[test][4] START STREAMING");

    if (!camera_service.startStreaming(
            rtsp_url)) {

        RKCAM_LOGE(
            "[test] start streaming failed");

        camera_service.stop();
        return 1;
    }

    if (!sleepSeconds(1)) {
        camera_service.stop();
        return 0;
    }

    RKCAM_LOGI(
        "[test] streaming_state=%d",
        static_cast<int>(
            camera_service.streamingState()));

    if (!sleepSeconds(4)) {
        camera_service.stop();
        return 0;
    }


    /*
     * ============================================================
     * 6. Start recording while streaming
     *
     * Expected:
     *   record=1 stream=1
     *   encode gate already ON
     *   must NOT turn off/restart MPP
     *
     * A new IDR is allowed for recording start.
     * ============================================================
     */
    RKCAM_LOGI(
        "[test][5] START RECORDING #2 "
        "WHILE STREAMING");

    if (!camera_service.startRecording(
            record_file_2)) {

        RKCAM_LOGE(
            "[test] start recording #2 failed");

        camera_service.stop();
        return 1;
    }

    if (!sleepSeconds(1)) {
        camera_service.stop();
        return 0;
    }

    RKCAM_LOGI(
        "[test] states: recording=%d streaming=%d",
        static_cast<int>(
            camera_service.recordingState()),
        static_cast<int>(
            camera_service.streamingState()));

    if (!sleepSeconds(4)) {
        camera_service.stop();
        return 0;
    }


    /*
     * ============================================================
     * 7. Stop recording, streaming must continue
     *
     * THIS IS A CRITICAL TEST.
     *
     * Expected:
     *   record=0 stream=1
     *   encode gate MUST remain ON
     *   MPP must keep encoding
     *   RTSP must keep sending
     * ============================================================
     */
    RKCAM_LOGI(
        "[test][6] STOP RECORDING #2 "
        "BUT KEEP STREAMING");

    if (!camera_service.stopRecording()) {
        RKCAM_LOGE(
            "[test] stop recording #2 failed");

        camera_service.stop();
        return 1;
    }

    if (!sleepSeconds(5)) {
        camera_service.stop();
        return 0;
    }

    RKCAM_LOGI(
        "[test] streaming should still be running: "
        "state=%d",
        static_cast<int>(
            camera_service.streamingState()));


    /*
     * ============================================================
     * 8. Start recording again while streaming
     *
     * This checks that MPP stays warm and can dynamically
     * insert another IDR.
     * ============================================================
     */
    const std::string record_file_3 =
        "/userdata/rkcam/output/"
        "gate_record_03.mp4";

    RKCAM_LOGI(
        "[test][7] START RECORDING #3 "
        "WHILE STREAMING");

    if (!camera_service.startRecording(
            record_file_3)) {

        RKCAM_LOGE(
            "[test] start recording #3 failed");

        camera_service.stop();
        return 1;
    }

    if (!sleepSeconds(5)) {
        camera_service.stop();
        return 0;
    }


    /*
     * ============================================================
     * 9. Stop streaming while recording continues
     *
     * ANOTHER CRITICAL TEST.
     *
     * Expected:
     *   record=1 stream=0
     *   encode gate MUST remain ON
     *   MP4 recording must continue
     * ============================================================
     */
    RKCAM_LOGI(
        "[test][8] STOP STREAMING "
        "BUT KEEP RECORDING");

    if (!camera_service.stopStreaming()) {
        RKCAM_LOGE(
            "[test] stop streaming failed");

        camera_service.stop();
        return 1;
    }

    if (!sleepSeconds(5)) {
        camera_service.stop();
        return 0;
    }

    RKCAM_LOGI(
        "[test] recording should still be running: "
        "state=%d",
        static_cast<int>(
            camera_service.recordingState()));


    /*
     * ============================================================
     * 10. Stop last consumer
     *
     * Expected:
     *   record=0 stream=0
     *   encode gate ON -> OFF
     * ============================================================
     */
    RKCAM_LOGI(
        "[test][9] STOP RECORDING #3");

    if (!camera_service.stopRecording()) {
        RKCAM_LOGE(
            "[test] stop recording #3 failed");

        camera_service.stop();
        return 1;
    }


    /*
     * ============================================================
     * 11. Final preview only
     *
     * Give the encode branch enough time to naturally drain.
     *
     * Expected:
     *   preview counters keep increasing
     *   record_rga/mpp may increase a few frames,
     *   then stay completely unchanged.
     * ============================================================
     */
    RKCAM_LOGI(
        "[test][10] FINAL PREVIEW ONLY: "
        "8 seconds");

    if (!sleepSeconds(8)) {
        camera_service.stop();
        return 0;
    }


    /*
     * ============================================================
     * 12. Stop whole pipeline
     * ============================================================
     */
    RKCAM_LOGI(
        "[test] CameraService stopping");

    camera_service.stop();

    RKCAM_LOGI(
        "====================================================");

    RKCAM_LOGI(
        "[test] Encode Gate integration test finished");

    RKCAM_LOGI(
        "====================================================");

    return 0;
}