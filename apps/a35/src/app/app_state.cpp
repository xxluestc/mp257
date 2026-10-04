#include "app/services.hpp"
std::atomic<int> g_running = 1;
std::atomic<int> g_led_alert = 0;
std::atomic<int> g_radar_npu_alert = 0; /* 雷达+NPU 确认告警 */
std::atomic<int> g_imu_fall_alert = 0;  /* IMU 摔倒告警 */
std::atomic<int> g_v2x_alert = 0;       /* V2X 告警 */
std::atomic<uint64_t> g_imu_fall_time_us = 0;
std::atomic<uint64_t> g_last_v2x_audio_us = 0;
std::atomic<uint64_t> g_pending_fall_dvr_us{0};

/* 可在命令行调整的雷达阈值 */
float g_ttc_threshold = TTC_THRESHOLD_DEFAULT;
float g_dist_threshold = DIST_THRESHOLD_DEFAULT;
float g_angle_left_threshold = ANGLE_LEFT_DEFAULT;
float g_angle_right_threshold = ANGLE_RIGHT_DEFAULT;
float g_angle_filter_alpha = ANGLE_FILTER_ALPHA_DEFAULT;
float g_angle_direction_sign = ANGLE_DIRECTION_SIGN_DEFAULT;
int g_direction_stable_samples = DIRECTION_STABLE_SAMPLES_DEFAULT;
char g_radar_log_dir[PATH_MAX] = RADAR_LOG_DIR_DEFAULT;
char g_ble_led_uart[PATH_MAX] = BLE_LED_UART_DEFAULT;
bool g_ble_led_enabled = true;
char g_dvr_base_dir[DVR_PATH_CAPACITY] = DVR_BASE_DIR_DEFAULT;
char g_dvr_mount_dir[DVR_PATH_CAPACITY] = DVR_MOUNT_DIR_DEFAULT;
char g_dvr_mount_parent[DVR_PATH_CAPACITY] = "/run/media";

int g_test_fall_delay_sec = 0;
int g_test_fall_count = 1;
int g_test_fall_interval_sec = 45;
int g_test_v2x_delay_sec = 0;
char g_test_v2x_direction[32] = "left_front";
