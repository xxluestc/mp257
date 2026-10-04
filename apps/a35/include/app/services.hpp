#pragma once
#include <atomic>
#include <cstdint>
#include <limits.h>
#include <sys/time.h>
#define UART_DEVICE "/dev/ttySTM1"
#define BAUDRATE 921600
#define V2X_AUDIO_COOLDOWN_US 2000000ULL
#define TTC_THRESHOLD_DEFAULT 2.5f
#define DIST_THRESHOLD_DEFAULT 3
#define HEAD_CMD 0x58
#define HEAD_REPLY 0x59
#define HEAD_REPORT 0x5A
#define TYPE_BSD 7
#define TARGET_TIMEOUT_S 3
#define MAX_RADAR_OBJECTS 8
#define ANGLE_LEFT_DEFAULT -10.0f
#define ANGLE_RIGHT_DEFAULT 10.0f
#define ANGLE_FILTER_ALPHA_DEFAULT 0.35f
#define DIRECTION_STABLE_SAMPLES_DEFAULT 3
#define DIRECTION_HYSTERESIS_DEG 2.0f
#define ANGLE_DIRECTION_SIGN_DEFAULT -1.0f
#define RADAR_LOG_DIR_DEFAULT "/usr/local/helmet/radar_experiments"
#define BLE_LED_UART_DEFAULT "/dev/ttySTM0"

/* LED */
#define GPIO_CHIP_DEV "/dev/gpiochip3"
#define GPIO_LED_LINE 11
#define LED_BLINK_ON_MS 200
#define LED_BLINK_OFF_MS 200

/* DVR */
#define DVR_BASE_DIR_DEFAULT "/run/media/mmcblk0p1/dvr"
#define DVR_MOUNT_DIR_DEFAULT "/run/media/mmcblk0p1"
#define DVR_PATH_CAPACITY 768
#define DVR_SAVE_BEFORE_SEC 15
#define DVR_SAVE_AFTER_SEC 15
#define DVR_CAPTURE_FPS 25
#define DVR_MAX_EVENT_SPAN_SEC 60

/* RPMsg (M33 IMU/V2X alerts) */
#define RPMSG_DEVICE "/dev/ttyRPMSG0"
#define RPMSG_READY_MSG "v2x_imu_alert_reader_ready\n"
#define RPMSG_BAUD B115200

/* Audio alert files */
#define AUDIO_FALL "/xxl/camera_detect/sounds/fall_alert.wav"
#define AUDIO_COLLISION "/xxl/camera_detect/sounds/collision_alert.wav"
#define AUDIO_V2X_NEARBY "/xxl/camera_detect/sounds/v2x_nearby.wav"
#define AUDIO_V2X_LEFT_FRONT "/xxl/camera_detect/sounds/v2x_left_front.wav"
#define AUDIO_V2X_RIGHT_FRONT "/xxl/camera_detect/sounds/v2x_right_front.wav"
#define AUDIO_V2X_LEFT "/xxl/camera_detect/sounds/v2x_left.wav"
#define AUDIO_V2X_RIGHT "/xxl/camera_detect/sounds/v2x_right.wav"
#define AUDIO_RECORDING_COMPLETE "/xxl/camera_detect/sounds/recording_complete.wav"

/* HUD / App 转发地址（与 v2x_alert_link.sh 一致） */
#define HUD_INPUT_IP "127.0.0.1"
#define HUD_INPUT_PORT 8890

#pragma pack(push, 1)

typedef struct {
    int8_t range_val, angle_val, velo_val, objId;
} bsd_obj_t;

typedef struct {
    uint16_t obj_num, reserved;
    bsd_obj_t obj[MAX_RADAR_OBJECTS];
} bsd_det_t;

#pragma pack(pop)

typedef enum {
    RADAR_DIR_UNKNOWN = 0,
    RADAR_DIR_LEFT,
    RADAR_DIR_CENTER,
    RADAR_DIR_RIGHT
} radar_direction_t;

typedef struct {
    int obj_id;
    float distance;
    float velocity;
    float angle;
    float filtered_angle;
    float ttc;
    radar_direction_t direction;
} radar_target_t;

typedef struct {
    int has_target;
    int obj_count;
    radar_target_t targets[MAX_RADAR_OBJECTS];
    int dangerous_index;
    int dangerous_obj_id;
    float min_distance;
    int approaching;
    float min_ttc;
    int should_alert;
} radar_result_t;

typedef struct {
    int valid;
    float filtered_angle;
    radar_direction_t stable_direction;
    radar_direction_t candidate_direction;
    int candidate_count;
    uint64_t last_seen_ms;
} radar_direction_filter_t;

extern std::atomic<int> g_running;
extern std::atomic<int> g_led_alert;
extern std::atomic<int> g_radar_npu_alert;
extern std::atomic<int> g_imu_fall_alert;
extern std::atomic<int> g_v2x_alert;
extern std::atomic<uint64_t> g_imu_fall_time_us;
extern std::atomic<uint64_t> g_last_v2x_audio_us;
extern std::atomic<uint64_t> g_pending_fall_dvr_us;
extern float g_ttc_threshold;
extern float g_dist_threshold;
extern float g_angle_left_threshold;
extern float g_angle_right_threshold;
extern float g_angle_filter_alpha;
extern float g_angle_direction_sign;
extern int g_direction_stable_samples;
extern char g_radar_log_dir[PATH_MAX];
extern char g_ble_led_uart[PATH_MAX];
extern bool g_ble_led_enabled;
extern char g_dvr_base_dir[DVR_PATH_CAPACITY];
extern char g_dvr_mount_dir[DVR_PATH_CAPACITY];
extern char g_dvr_mount_parent[DVR_PATH_CAPACITY];
extern int g_test_fall_delay_sec;
extern int g_test_fall_count;
extern int g_test_fall_interval_sec;
extern int g_test_v2x_delay_sec;
extern char g_test_v2x_direction[32];

double boot_time_seconds();
void startup_mark(const char *milestone);
int gpio_init();
void gpio_deinit();
void *led_thread(void *arg);
int set_uart(int fd, int baudrate);
int send_cmd(int fd, uint8_t group, uint8_t cmd, const uint8_t *params, int param_len);
uint8_t calc_sum8(const uint8_t *data, int len);
void flush_rx(int fd);
int radar_init(int fd);
int process_radar_frame(const uint8_t *frame, int frame_len, radar_result_t *out);
const char *radar_direction_name(radar_direction_t direction);
void radar_telemetry_publish(const radar_result_t *radar, int fusion_alert, bool available = true);
void radar_telemetry_publish_empty();
void radar_telemetry_close();
void sensor_telemetry_close();
void sensor_event_log(const char *source, const char *event_type, const char *status,
                      const char *event_id, const char *label, float score, int count, int seq,
                      const char *reason, const char *details);
int finalize_dvr_storage_paths();
void storage_try_initialize();
void play_alert_sound(const char *type);
void play_recording_complete_sound();
void *rpmsg_thread(void *arg);
void *test_fall_thread(void *arg);
void *test_v2x_thread(void *arg);
