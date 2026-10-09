// 应用入口与融合控制：消费 Radar/NPU 结果，统一决定告警、方向灯和录像触发。
// 采集、推理和编码在独立工作线程执行；本文件负责把这些链路组织起来。
#include "app/services.hpp"
#include "ble_risk_output.h"
#include "nav_tts.h"
#include "radar/radar_worker.hpp"
#include "runtime/fusion_state.hpp"
#include "runtime/worker_group.hpp"
#include "runtime/video_pipeline.hpp"
#include <cstdio>
#include <cstring>
#include <csignal>
#include <memory>

int parse_arguments(int argc, char **argv, helmet::VideoConfig &video, std::string &radar_device);

namespace {
volatile sig_atomic_t stop_requested = 0;

void request_stop(int) {
    // 信号处理函数只置标志；日志、设备清理和 join 交给正常执行流完成。
    stop_requested = 1;
}

class ApplicationResources {
  public:
    ApplicationResources() = default;
    ApplicationResources(const ApplicationResources &) = delete;
    ApplicationResources &operator=(const ApplicationResources &) = delete;

    ~ApplicationResources() {
        // run() 中最先创建、最后析构，保证工作线程退出后再释放外围服务。
        ble_risk_shutdown();
        nav_tts_stop();
        gpio_deinit();
        radar_telemetry_close();
        sensor_telemetry_close();
    }
};

void on_video_event(const char *event, const std::string &details) {
    // 此回调由各视频工作线程直接调用，可能并发；日志接口自行同步，音频只入队。
    // 它负责事件呈现，不修改 Fusion 的视觉确认或雷达风险状态。
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

    // 声明顺序决定异常展开时的析构顺序：workers 先回收，队列和 video 随后销毁。
    // [&] 捕获的队列与设备路径因此始终活到 radar_receive_loop 退出之后。
    helmet::VideoPipeline video(std::move(config), on_video_event);
    helmet::BoundedQueue<RadarObservation> radar_results(8);
    helmet::WorkerGroup workers(g_running);
    workers.start([&] { radar_receive_loop(radar_device, radar_results); });
    workers.start([] { led_thread(nullptr); });
    workers.start([] { rpmsg_thread(nullptr); });
    workers.start([] { test_fall_thread(nullptr); });
    workers.start([] { test_v2x_thread(nullptr); });
    video.start();
    // 启动标记表示线程已创建；首帧、首份雷达报告和模型加载另有独立标记。
    startup_mark("fusion_risk_core_ready");
    startup_mark("radar_fusion_runtime_ready");

    helmet::VisionGate vision;
    helmet::NpuObservation npu;
    RadarObservation radar_state;
    bool previous_alert = false;
    uint64_t next_heartbeat = 0, next_storage_check = 0, next_trigger = 0;
    while (!stop_requested) {
        uint64_t now = helmet::monotonic_us();
        if (now >= next_storage_check) {
            storage_try_initialize();
            next_storage_check = now + 2000000;
        }
        // 按采集时间更新视觉确认。推理完成时间不能代替采集时间判断数据新鲜度。
        helmet::NpuObservation observation;
        while (video.observation(observation)) {
            vision.update(observation.timestamp_us, observation.has_road_user);
            npu = std::move(observation);
            sensor_event_log("camera_npu", "npu_inference", npu.has_road_user ? "target" : "clear",
                             nullptr, npu.label.c_str(), npu.score,
                             static_cast<int>(npu.road_count), -1, nullptr,
                             vision.confirmed() ? "confirmed=1" : "confirmed=0");
        }
        // 逐项取出已到达的报告，最终使用最新状态；无新报告时保留旧值供时效判断。
        RadarObservation report;
        bool updated = false;
        while (radar_results.pop(report, std::chrono::milliseconds(0))) {
            radar_state = report;
            updated = true;
        }
        // 排空队列后重新取时间，避免刚到达的报告时间晚于本轮开始时的 now。
        now = helmet::monotonic_us();
        bool radar_fresh = radar_state.timestamp_us && now >= radar_state.timestamp_us &&
                           now - radar_state.timestamp_us <= 3000000;
        const auto &result = radar_state.result;
        // 雷达必须先满足风险条件；视觉有效时再确认道路使用者，视觉失效时退回雷达。
        bool alert =
            vision.alert(radar_fresh && result.should_alert, video.camera_available(now), now);
        g_radar_npu_alert = alert;
        // 原子邮箱一次取走待处理摔倒事件；RPMsg 线程不直接操作 DVR 的会话状态。
        auto fall = g_pending_fall_dvr_us.exchange(0);
        if (fall && !video.trigger(fall))
            sensor_event_log("a35_dvr", "recording", "failed", nullptr, nullptr, -1, -1, -1,
                             "trigger_queue_full", "fall");
        uint64_t fall_at = g_imu_fall_time_us.load();
        if (fall_at && now >= fall_at && now - fall_at >= 5000000)
            g_imu_fall_alert = 0;
        g_led_alert = alert || g_imu_fall_alert;
        // 风险上升沿播放提示；持续风险每秒投递一次触发，用于延长录像后段。
        if (alert && !previous_alert)
            play_alert_sound("collision");
        if (alert && (!previous_alert || now >= next_trigger)) {
            if (!video.trigger(now))
                sensor_event_log("a35_dvr", "recording", "failed", nullptr, nullptr, -1, -1, -1,
                                 "trigger_queue_full", "radar_npu_collision");
            next_trigger = now + 1000000;
        }
        // 方向灯使用雷达选出的危险目标方向，视觉结果只参与风险确认。
        BleRiskState direction = BLE_RISK_CLEAR;
        if (alert && result.dangerous_index >= 0 && result.dangerous_index < result.obj_count) {
            auto side = result.targets[result.dangerous_index].direction;
            direction = side == RADAR_DIR_LEFT    ? BLE_RISK_LEFT
                        : side == RADAR_DIR_RIGHT ? BLE_RISK_RIGHT
                                                  : BLE_RISK_CENTER;
        }
        ble_risk_update(direction);
        // 心跳维持面板更新，但过期雷达必须发布 unavailable，不能伪装成正常无目标。
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
    // 先通知后台线程停止，再等待各链路退出；外围句柄由 resources 在最后释放。
    g_running = 0;
    video.stop();
    workers.stop();
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
