#include "app/services.hpp"
#include "ble_risk_output.h"
#include "nav_tts.h"
#include "radar/radar_worker.hpp"
#include "runtime/fusion_state.hpp"
#include "runtime/linux_resources.hpp"
#include "runtime/video_pipeline.hpp"
#include <cstdio>
#include <cstring>
#include <csignal>
#include <memory>

int parse_arguments(int argc, char **argv, helmet::VideoConfig &video, std::string &radar_device);

namespace {
volatile sig_atomic_t stop_requested = 0;

void request_stop(int) {
    stop_requested = 1;
}

class ApplicationResources {
  public:
    ~ApplicationResources() {
        ble_risk_shutdown();
        nav_tts_stop();
        gpio_deinit();
        radar_telemetry_close();
        sensor_telemetry_close();
    }
};

class StopOnExit {
  public:
    ~StopOnExit() {
        g_running = 0;
    }
};

void on_video_event(const char *event, const std::string &details) {
    printf("[VIDEO] %s %s\n", event, details.c_str());
    if (strcmp(event, "camera_first_frame") == 0)
        startup_mark("camera_first_frame");
    if (strcmp(event, "npu_attached") == 0)
        startup_mark("npu_model_load_done");
    const char *status = "info";
    const char *type = event;
    const char *source = "video_runtime";
    if (strstr(event, "_failed") != nullptr)
        status = "failed";
    if (strncmp(event, "recording_", 10) == 0 || strncmp(event, "encoder_", 8) == 0)
        source = "a35_dvr";
    if (strcmp(event, "recording_started") == 0) {
        type = "recording";
        status = "triggered";
    } else if (strcmp(event, "encoding_started") == 0) {
        source = "a35_dvr";
        type = "encoding";
        status = "started";
    } else if (strcmp(event, "recording_rejected") == 0 ||
               strcmp(event, "recording_overflow") == 0 || strcmp(event, "encoder_failed") == 0) {
        type = "recording";
        status = "failed";
    }
    if (strcmp(event, "recording_saved") == 0) {
        type = "recording";
        status = "saved";
        play_recording_complete_sound();
    } else if (strcmp(event, "recording_failed") == 0) {
        type = "recording";
        status = "failed";
    }
    sensor_event_log(source, type, status, nullptr, nullptr, -1, -1, -1, nullptr, details.c_str());
}

int run(helmet::VideoConfig config, const std::string &radar_device) {
    ApplicationResources resources;
    ble_risk_configure(g_ble_led_uart, g_ble_led_enabled);
    gpio_init();
    storage_try_initialize();
    nav_tts_set_danger_text_handler(nav_tts_speak_danger);
    if (nav_tts_start() != 0)
        fprintf(stderr, "[NAV] Receiver unavailable\n");

    helmet::VideoPipeline video(std::move(config), on_video_event);
    helmet::BoundedQueue<RadarObservation> radar_results(8);
    helmet::JoiningThread radar([&] { radar_receive_loop(radar_device, radar_results); });
    // Construct the stop guard immediately after each group of thread owners:
    // its destructor runs before joins even if later startup throws.
    StopOnExit radar_stop;
    helmet::JoiningThread led([] { led_thread(nullptr); });
    StopOnExit led_stop;
    helmet::JoiningThread rpmsg([] { rpmsg_thread(nullptr); });
    StopOnExit rpmsg_stop;
    helmet::JoiningThread test_fall([] { test_fall_thread(nullptr); });
    StopOnExit fall_stop;
    helmet::JoiningThread test_v2x([] { test_v2x_thread(nullptr); });
    StopOnExit v2x_stop;
    video.start();
    startup_mark("fusion_risk_core_ready");
    startup_mark("radar_fusion_runtime_ready");

    helmet::VisionGate vision;
    helmet::NpuObservation npu;
    RadarObservation radar_state;
    bool previous_alert = false;
    uint64_t next_heartbeat = 0, next_storage_check = 0, next_trigger = 0;
    while (!stop_requested) {
        const uint64_t now = helmet::monotonic_us();
        if (now >= next_storage_check) {
            storage_try_initialize();
            next_storage_check = now + 2000000;
        }
        helmet::NpuObservation observation;
        while (video.observation(observation)) {
            vision.update(observation.timestamp_us, observation.has_road_user);
            npu = std::move(observation);
            sensor_event_log("camera_npu", "npu_inference", npu.has_road_user ? "target" : "clear",
                             nullptr, npu.label.c_str(), npu.score,
                             static_cast<int>(npu.road_count), -1, nullptr,
                             vision.confirmed() ? "confirmed=1" : "confirmed=0");
        }
        RadarObservation report;
        bool updated = false;
        while (radar_results.pop(report, std::chrono::milliseconds(0))) {
            radar_state = report;
            updated = true;
        }
        bool radar_fresh = radar_state.timestamp_us && now >= radar_state.timestamp_us &&
                           now - radar_state.timestamp_us <= 3000000;
        const auto &result = radar_state.result;
        bool alert =
            vision.alert(radar_fresh && result.should_alert, video.camera_available(now), now);
        g_radar_npu_alert = alert;
        auto fall = g_pending_fall_dvr_us.exchange(0);
        if (fall && !video.trigger(fall))
            sensor_event_log("a35_dvr", "recording", "failed", nullptr, nullptr, -1, -1, -1,
                             "trigger_queue_full", "fall");
        uint64_t fall_at = g_imu_fall_time_us.load();
        if (fall_at && now >= fall_at && now - fall_at >= 5000000)
            g_imu_fall_alert = 0;
        g_led_alert = alert || g_imu_fall_alert;
        if (alert && !previous_alert)
            play_alert_sound("collision");
        if (alert && (!previous_alert || now >= next_trigger)) {
            if (!video.trigger(now))
                sensor_event_log("a35_dvr", "recording", "failed", nullptr, nullptr, -1, -1, -1,
                                 "trigger_queue_full", "radar_npu_collision");
            next_trigger = now + 1000000;
        }
        BleRiskState direction = BLE_RISK_CLEAR;
        if (alert && result.dangerous_index >= 0 && result.dangerous_index < result.obj_count) {
            auto side = result.targets[result.dangerous_index].direction;
            direction = side == RADAR_DIR_LEFT    ? BLE_RISK_LEFT
                        : side == RADAR_DIR_RIGHT ? BLE_RISK_RIGHT
                                                  : BLE_RISK_CENTER;
        }
        ble_risk_update(direction);
        if (updated || alert != previous_alert || now >= next_heartbeat) {
            if (radar_fresh)
                radar_telemetry_publish(&result, alert);
            else
                radar_telemetry_publish_empty();
            next_heartbeat = now + 1000000;
        }
        previous_alert = alert;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    g_running = 0;
    video.stop();
    radar.join();
    led.join();
    rpmsg.join();
    test_fall.join();
    test_v2x.join();
    g_led_alert = 0;
    return 0;
}
} // namespace

int main(int argc, char **argv) {
    helmet::VideoConfig video;
    std::string radar_device;
    int parsed = parse_arguments(argc, argv, video, radar_device);
    if (parsed != 0)
        return parsed == 1 ? 0 : parsed;
    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);
    setvbuf(stderr, nullptr, _IOLBF, BUFSIZ);
    startup_mark("radar_fusion_main_enter");
    try {
        return run(std::move(video), radar_device);
    } catch (const std::exception &error) {
        g_running = 0;
        fprintf(stderr, "[FUSION] Startup/runtime failure: %s\n", error.what());
        return 1;
    }
}
