/**
 * radar_fusion.cpp - 雷达 + 摄像头 NPU 融合 + DVR 行车记录
 *
 * 整体数据流 (见 docs/DATA_FLOW.md):
 *
 *   [雷达模块 /dev/ttySTM1] --BSD目标--> [主循环]
 *                              |
 *   [摄像头 /dev/video7] --MJPEG帧--> [DVR缓冲] ----------> [MP4保存]
 *                              |
 *                              v
 *                    [每10帧取1帧 NPU推理]
 *                              |
 *                              v
 *                    [判断是否为道路使用者]
 *                              |
 *                              v
 *        [雷达告警 + NPU确认] -> LED闪烁 + 碰撞告警 + 触发DVR保存
 *
 *   [M33核 /dev/ttyRPMSG0] --IMU摔倒/V2X告警--> [RPMsg线程] -> 触发DVR保存/语音播报
 *
 *   [手机APP UDP 8888] --navi/danger_tts/alert--> [nav_tts线程]
 *                              |
 *                              +-- navi: 刷新OLED, <=50m时语音播报转向
 *                              +-- danger_tts(trigger): 异常路况语音播报
 *                              +-- alert: 固定预警提示音
 *
 * 功能:
 *   1. 雷达 BSD 目标检测 + 摄像头 NPU 道路用户验证
 *   2. PD11 LED 告警闪烁
 *   3. DVR 行车记录:
 *      - TARGET_ON → 开始缓冲 MJPEG 帧到 TF 卡
 *      - COLLISION + NPU 确认 → 保存前后各 15 秒为 MP4
 *      - TARGET_OFF → 清理缓冲(无触发时)
 *
 * 编译: make radar-fusion CC=... CXX=...
 * 运行: ./radar_fusion [-d /dev/video7] [-u /dev/ttySTM1] [-c 0.60]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <pthread.h>
#include <setjmp.h>
#include <time.h>
#include <math.h>
#include <dirent.h>
#include <limits.h>
#include <float.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <linux/gpio.h>
#include <jpeglib.h>

#include "camera.h"
#include "ble_risk_output.h"
#include "npu_detect.h"
#include "nav_tts.h"

/* ======================== 配置常量 ======================== */
#define UART_DEVICE            "/dev/ttySTM1"
#define BAUDRATE               921600
#define TTC_THRESHOLD_DEFAULT  2.5f
#define DIST_THRESHOLD_DEFAULT 3
#define HEAD_CMD               0x58
#define HEAD_REPLY             0x59
#define HEAD_REPORT            0x5A
#define TYPE_BSD               7
#define ROAD_USER_CLASSES      {1,2,3,4,6,7,8}
#define NPU_CONFIRM_FRAMES     2
#define NPU_DENY_FRAMES        3
#define TARGET_TIMEOUT_S       3
#define MAX_RADAR_OBJECTS      8
#define ANGLE_LEFT_DEFAULT    -10.0f
#define ANGLE_RIGHT_DEFAULT    10.0f
#define ANGLE_FILTER_ALPHA_DEFAULT 0.35f
#define DIRECTION_STABLE_SAMPLES_DEFAULT 3
#define DIRECTION_HYSTERESIS_DEG 2.0f
#define ANGLE_DIRECTION_SIGN_DEFAULT -1.0f
#define RADAR_LOG_DIR_DEFAULT  "/run/media/mmcblk0p1/dvr/radar_experiments"
#define BLE_LED_UART_DEFAULT   "/dev/ttySTM0"

/* LED */
#define GPIO_CHIP_DEV          "/dev/gpiochip3"
#define GPIO_LED_LINE          11
#define LED_BLINK_ON_MS        200
#define LED_BLINK_OFF_MS       200

/* DVR */
#define DVR_BASE_DIR           "/run/media/mmcblk0p1/dvr"
#define DVR_BUFFER_DIR         "/run/media/mmcblk0p1/dvr/.buffer"
#define DVR_SAVE_BEFORE_SEC    15
#define DVR_SAVE_AFTER_SEC     15
#define DVR_CAPTURE_FPS        25
#define DVR_CAPTURE_INTERVAL_US (1000000 / DVR_CAPTURE_FPS)

/* RPMsg (M33 IMU/V2X alerts) */
#define RPMSG_DEVICE           "/dev/ttyRPMSG0"
#define RPMSG_READY_MSG        "v2x_imu_alert_reader_ready\n"
#define RPMSG_BAUD             B115200

/* Audio alert files */
#define AUDIO_FALL             "/xxl/camera_detect/sounds/fall_alert.wav"
#define AUDIO_COLLISION        "/xxl/camera_detect/sounds/collision_alert.wav"
#define AUDIO_V2X_NEARBY       "/xxl/camera_detect/sounds/v2x_nearby.wav"
#define AUDIO_V2X_LEFT_FRONT   "/xxl/camera_detect/sounds/v2x_left_front.wav"
#define AUDIO_V2X_RIGHT_FRONT  "/xxl/camera_detect/sounds/v2x_right_front.wav"
#define AUDIO_V2X_LEFT         "/xxl/camera_detect/sounds/v2x_left.wav"
#define AUDIO_V2X_RIGHT        "/xxl/camera_detect/sounds/v2x_right.wav"
#define AUDIO_RECORDING_COMPLETE "/xxl/camera_detect/sounds/recording_complete.wav"

/* HUD / App 转发地址（与 v2x_alert_link.sh 一致） */
#define HUD_INPUT_IP           "127.0.0.1"
#define HUD_INPUT_PORT         8890

/* ======================== 雷达协议结构体 ======================== */
#pragma pack(push, 1)
typedef struct { int8_t range_val, angle_val, velo_val, objId; } bsd_obj_t;
typedef struct { uint16_t obj_num, reserved; bsd_obj_t obj[MAX_RADAR_OBJECTS]; } bsd_det_t;
#pragma pack(pop)

/* ======================== DVR 帧记录 ======================== */
typedef struct {
    uint64_t timestamp_us;   /* 从启动开始的时间戳 */
    off_t     file_offset;   /* 在帧文件中的偏移 */
    uint32_t  jpeg_size;     /* JPEG 数据大小 */
    uint32_t  frame_index;   /* 帧序号 */
} dvr_frame_entry_t;

#define DVR_MAX_FRAMES (DVR_SAVE_BEFORE_SEC * DVR_CAPTURE_FPS * 2)

/* ======================== 全局状态 ======================== */
static volatile int g_running    = 1;
static volatile int g_led_alert  = 0;
static volatile int g_radar_npu_alert = 0;   /* 雷达+NPU 确认告警 */
static volatile int g_imu_fall_alert  = 0;   /* IMU 摔倒告警 */
static volatile int g_v2x_alert       = 0;   /* V2X 告警 */
static volatile uint64_t g_imu_fall_time_us = 0;
static volatile uint64_t g_last_v2x_audio_us = 0;
#define V2X_AUDIO_COOLDOWN_US 2000000ULL     /* V2X 语音 2 秒防连播 */
static int g_led_fd              = -1;
static int g_camera_ok_global    = 0;        /* 供 RPMsg 线程使用 */
static char g_last_road_user_label[32] = "unknown";
static float g_last_road_user_score    = 0.0f;
static struct timeval g_t_start;             /* 程序启动时间 (全局) */

/* 可在命令行调整的雷达阈值 */
static float g_ttc_threshold  = TTC_THRESHOLD_DEFAULT;
static float g_dist_threshold = DIST_THRESHOLD_DEFAULT;
static float g_angle_left_threshold  = ANGLE_LEFT_DEFAULT;
static float g_angle_right_threshold = ANGLE_RIGHT_DEFAULT;
static float g_angle_filter_alpha = ANGLE_FILTER_ALPHA_DEFAULT;
static float g_angle_direction_sign = ANGLE_DIRECTION_SIGN_DEFAULT;
static int   g_direction_stable_samples = DIRECTION_STABLE_SAMPLES_DEFAULT;
static char  g_radar_log_dir[PATH_MAX] = RADAR_LOG_DIR_DEFAULT;
static char  g_ble_led_uart[PATH_MAX] = BLE_LED_UART_DEFAULT;
static bool  g_ble_led_enabled = true;

/* ======================== 信号处理 ======================== */
static void sig_handler(int sig) { (void)sig; g_running = 0; }

/* ======================== GPIO LED (字符设备 API) ======================== */

/**
 * @brief 初始化 GPIO LED（PD11）
 * @return 0 成功，-1 失败
 *
 * 通过 /dev/gpiochip3 请求 GPIO 线为输出模式，用于后续告警闪烁。
 */
static int gpio_init(void) {
    int chip_fd = open(GPIO_CHIP_DEV, O_RDONLY);
    if (chip_fd < 0) {
        fprintf(stderr, "[LED] Cannot open %s: %s\n", GPIO_CHIP_DEV, strerror(errno));
        return -1;
    }
    struct gpiohandle_request req;
    memset(&req, 0, sizeof(req));
    req.lineoffsets[0] = GPIO_LED_LINE;
    req.flags = GPIOHANDLE_REQUEST_OUTPUT;
    req.default_values[0] = 0;
    req.lines = 1;
    strncpy(req.consumer_label, "radar_fusion", sizeof(req.consumer_label) - 1);
    if (ioctl(chip_fd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) {
        fprintf(stderr, "[LED] GPIO line request failed: %s\n", strerror(errno));
        close(chip_fd);
        return -1;
    }
    close(chip_fd);
    g_led_fd = req.fd;
    printf("[LED] GPIO PD11 ready\n");
    return 0;
}

/**
 * @brief 设置 GPIO LED 电平
 * @param val 0 熄灭，非 0 点亮
 */
static void gpio_set(int val) {
    if (g_led_fd < 0) return;
    struct gpiohandle_data data;
    data.values[0] = (uint8_t)(val ? 1 : 0);
    ioctl(g_led_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data);
}

/**
 * @brief 释放 GPIO LED 资源
 *
 * 熄灭 LED 并关闭 GPIO 线句柄。
 */
static void gpio_deinit(void) {
    if (g_led_fd >= 0) { gpio_set(0); close(g_led_fd); g_led_fd = -1; }
}

static void *led_thread(void *arg) {
    (void)arg;
    while (g_running) {
        if (g_led_alert) {
            gpio_set(1); usleep(LED_BLINK_ON_MS  * 1000);
            gpio_set(0); usleep(LED_BLINK_OFF_MS * 1000);
        } else {
            gpio_set(0);
            usleep(100000);
        }
    }
    gpio_set(0);
    return NULL;
}

/* ======================== 设备冲突清理 ======================== */

/**
 * @brief 强制结束占用指定设备的进程
 * @param device 设备节点路径
 *
 * 使用 fuser 终止持有该设备的进程，避免摄像头/串口被占用导致打开失败。
 */
static void kill_device_holders(const char *device) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "fuser -k %s 2>/dev/null", device);
    if (system(cmd) == 0) {
        printf("[CLEANUP] Killed processes holding %s\n", device);
        usleep(500000);
    }
}

/* ======================== 串口 ======================== */

/**
 * @brief 配置串口参数
 * @param fd       串口文件描述符
 * @param baudrate 波特率，支持 9600 / 115200 / 921600
 * @return 0 成功，-1 失败
 *
 * 配置为 8N1、无流控、原始模式（非规范模式），读写超时 100ms。
 */
static int set_uart(int fd, int baudrate) {
    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) { perror("tcgetattr"); return -1; }
    speed_t speed;
    switch (baudrate) {
        case 9600: speed = B9600; break;
        case 115200: speed = B115200; break;
        case 921600: speed = B921600; break;
        default: fprintf(stderr, "Unsupported baudrate: %d\n", baudrate); return -1;
    }
    cfsetospeed(&tty, speed); cfsetispeed(&tty, speed);
    tty.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
    tty.c_cflag |= CS8 | CREAD | CLOCAL;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY | IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
    tty.c_oflag &= ~OPOST & ~ONLCR;
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_cc[VMIN] = 0; tty.c_cc[VTIME] = 1;
    if (tcsetattr(fd, TCSANOW, &tty) != 0) { perror("tcsetattr"); return -1; }
    tcflush(fd, TCIOFLUSH);
    return 0;
}

/**
 * @brief 计算 16 位累加和校验
 * @param data 数据指针
 * @param len  数据长度
 * @return 16 位校验和
 */
static uint16_t calc_sum16(const uint8_t *data, int len) {
    uint32_t sum = 0;
    for (int i = 0; i < len; i++) sum += data[i];
    return (uint16_t)(sum & 0xFFFF);
}

/**
 * @brief 计算 8 位累加和校验
 * @param data 数据指针
 * @param len  数据长度
 * @return 8 位校验和
 */
static uint8_t calc_sum8(const uint8_t *data, int len) {
    uint32_t sum = 0;
    for (int i = 0; i < len; i++) sum += data[i];
    return (uint8_t)(sum & 0xFF);
}

/**
 * @brief 向雷达模块发送命令帧
 * @param fd        串口文件描述符
 * @param group     命令组号
 * @param cmd       命令号
 * @param params    命令参数缓冲区
 * @param param_len 参数长度
 * @return 0 成功，-1 失败
 *
 * 帧格式：HEAD_CMD (1B) | group+cmd (1B) | param_len (1B) | params (nB) | sum16 (2B)
 */
static int send_cmd(int fd, uint8_t group, uint8_t cmd, const uint8_t *params, int param_len) {
    uint8_t frame[64];
    int idx = 0;
    frame[idx++] = HEAD_CMD;
    frame[idx++] = (group << 5) | (cmd & 0x1F);
    frame[idx++] = (uint8_t)param_len;
    if (params && param_len > 0) { memcpy(&frame[idx], params, param_len); idx += param_len; }
    uint16_t csum = calc_sum16(frame, idx);
    frame[idx++] = (uint8_t)(csum & 0xFF);
    frame[idx++] = (uint8_t)((csum >> 8) & 0xFF);
    int written = write(fd, frame, idx);
    if (written != idx) { fprintf(stderr, "radar write failed: %s\n", strerror(errno)); return -1; }
    tcdrain(fd);
    return 0;
}

/**
 * @brief 清空串口接收缓冲区的残余数据
 * @param fd 串口文件描述符
 */
static void flush_rx(int fd) { uint8_t tmp[256]; int n; while ((n = read(fd, tmp, sizeof(tmp))) > 0); }

/* ======================== JPEG 解码 (静默) ======================== */
struct my_jpeg_error { struct jpeg_error_mgr pub; jmp_buf setjmp_buf; };
static void my_jpeg_emit_message(j_common_ptr cinfo, int msg_level) { (void)cinfo; (void)msg_level; }
static void my_jpeg_error_exit(j_common_ptr cinfo) {
    struct my_jpeg_error *myerr = (struct my_jpeg_error *)cinfo->err;
    longjmp(myerr->setjmp_buf, 1);
}
static int jpeg_decode_rgb_silent(const unsigned char *jpeg_data, unsigned long jpeg_size,
                                   unsigned char *out_rgb, int *out_width, int *out_height) {
    struct jpeg_decompress_struct cinfo;
    struct my_jpeg_error jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = my_jpeg_error_exit;
    jerr.pub.emit_message = my_jpeg_emit_message;
    if (setjmp(jerr.setjmp_buf)) { jpeg_destroy_decompress(&cinfo); return -1; }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpeg_data, jpeg_size);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) { jpeg_destroy_decompress(&cinfo); return -1; }
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);
    *out_width = cinfo.output_width;
    *out_height = cinfo.output_height;
    int row_stride = cinfo.output_width * cinfo.output_components;
    unsigned char *row_ptr = out_rgb;
    while (cinfo.output_scanline < cinfo.output_height) {
        unsigned char *buf[1] = { row_ptr };
        jpeg_read_scanlines(&cinfo, buf, 1);
        row_ptr += row_stride;
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return 0;
}

static void resize_rgb(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        int sy = y * sh / dh;
        for (int x = 0; x < dw; x++) {
            int sx = x * sw / dw;
            int si = (sy * sw + sx) * 3;
            int di = (y * dw + x) * 3;
            dst[di]=src[si]; dst[di+1]=src[si+1]; dst[di+2]=src[si+2];
        }
    }
}

static int is_road_user(int class_index) {
    static const int road_classes[] = ROAD_USER_CLASSES;
    static const int count = sizeof(road_classes) / sizeof(road_classes[0]);
    for (int i = 0; i < count; i++) if (class_index == road_classes[i]) return 1;
    return 0;
}

/* ======================== 雷达协议处理 ======================== */

/**
 * @brief 雷达 BSD 处理结果
 */
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

static radar_direction_filter_t g_direction_filters[256];
static FILE *g_radar_csv = NULL;
static FILE *g_sensor_csv = NULL;
static pthread_mutex_t g_sensor_csv_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_radar_csv_path[PATH_MAX];
static char g_sensor_csv_path[PATH_MAX];
static char g_radar_state_path[PATH_MAX];
static uint64_t g_radar_last_publish_ms = 0;
static unsigned int g_radar_csv_write_count = 0;
static unsigned int g_sensor_csv_write_count = 0;

#define TELEMETRY_LOG_MAX_BYTES (20U * 1024U * 1024U)
#define TELEMETRY_LOG_BACKUPS   4
#define TELEMETRY_SIZE_CHECK_WRITES 256U

static const char *radar_direction_name(radar_direction_t direction) {
    switch (direction) {
        case RADAR_DIR_LEFT:   return "LEFT";
        case RADAR_DIR_CENTER: return "CENTER";
        case RADAR_DIR_RIGHT:  return "RIGHT";
        default:               return "UNKNOWN";
    }
}

static radar_direction_t classify_radar_direction(float angle,
                                                   radar_direction_t current) {
    /*
     * 在边界处加入滞回：已经处于 LEFT/RIGHT 时，目标必须明显回到 CENTER
     * 才允许切换，避免阈值附近一帧一跳。
     */
    if (current == RADAR_DIR_LEFT &&
        angle <= g_angle_left_threshold + DIRECTION_HYSTERESIS_DEG)
        return RADAR_DIR_LEFT;
    if (current == RADAR_DIR_RIGHT &&
        angle >= g_angle_right_threshold - DIRECTION_HYSTERESIS_DEG)
        return RADAR_DIR_RIGHT;

    if (angle <= g_angle_left_threshold) return RADAR_DIR_LEFT;
    if (angle >= g_angle_right_threshold) return RADAR_DIR_RIGHT;
    return RADAR_DIR_CENTER;
}

static radar_direction_t update_direction_filter(int obj_id, float raw_angle,
                                                 uint64_t now_ms,
                                                 float *filtered_angle_out) {
    radar_direction_filter_t *filter =
        &g_direction_filters[(unsigned int)obj_id & 0xFFU];

    if (!filter->valid || now_ms - filter->last_seen_ms > 5000ULL) {
        memset(filter, 0, sizeof(*filter));
        filter->valid = 1;
        filter->filtered_angle = raw_angle;
        filter->stable_direction =
            classify_radar_direction(
                g_angle_direction_sign * raw_angle, RADAR_DIR_UNKNOWN);
        filter->candidate_direction = filter->stable_direction;
    } else {
        filter->filtered_angle =
            g_angle_filter_alpha * raw_angle +
            (1.0f - g_angle_filter_alpha) * filter->filtered_angle;

        radar_direction_t candidate = classify_radar_direction(
            g_angle_direction_sign * filter->filtered_angle,
            filter->stable_direction);
        if (candidate == filter->stable_direction) {
            filter->candidate_direction = candidate;
            filter->candidate_count = 0;
        } else if (candidate == filter->candidate_direction) {
            filter->candidate_count++;
        } else {
            filter->candidate_direction = candidate;
            filter->candidate_count = 1;
        }

        if (filter->candidate_count >= g_direction_stable_samples) {
            filter->stable_direction = filter->candidate_direction;
            filter->candidate_count = 0;
        }
    }

    filter->last_seen_ms = now_ms;
    if (filtered_angle_out != NULL)
        *filtered_angle_out = filter->filtered_angle;
    return filter->stable_direction;
}

/**
 * @brief 解析 BSD 报告并计算是否需要告警
 * @param bsd 解析后的 BSD 检测数据
 * @return 雷达处理结果
 *
 * 告警条件：
 *   - 任一目标靠近且 TTC < g_ttc_threshold，或
 *   - 任一目标距离 <= g_dist_threshold
 *
 * 危险目标按 min(TTC/TTC阈值, 距离/距离阈值) 选择。该选择方式保留原告警
 * 条件，同时保证距离与 TTC 始终来自同一个 objId。
 */
static radar_result_t process_bsd_report(const bsd_det_t *bsd) {
    radar_result_t result = {0};
    int obj_count = bsd->obj_num;
    if (obj_count > MAX_RADAR_OBJECTS) obj_count = MAX_RADAR_OBJECTS;
    result.obj_count = obj_count;
    result.dangerous_index = -1;
    result.dangerous_obj_id = -1;
    result.min_distance = 9999.0f;
    result.min_ttc = 9999.0f;
    float best_risk = FLT_MAX;

    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);
    uint64_t now_ms = (uint64_t)tv_now.tv_sec * 1000ULL +
                      (uint64_t)tv_now.tv_usec / 1000ULL;

    for (int i = 0; i < obj_count; i++) {
        const bsd_obj_t *o = &bsd->obj[i];
        radar_target_t *target = &result.targets[i];
        target->obj_id = (int)(uint8_t)o->objId;
        target->distance = (float)o->range_val;
        target->velocity = (float)o->velo_val;
        target->angle = (float)o->angle_val;
        target->ttc = -1.0f;
        target->direction =
            update_direction_filter(target->obj_id, target->angle, now_ms,
                                    &target->filtered_angle);

        if (target->distance <= 0.0f)
            continue;

        result.has_target = 1;
        if (target->distance < result.min_distance)
            result.min_distance = target->distance;

        if (target->velocity < 0.0f) {
            target->ttc = target->distance / -target->velocity;
            if (target->ttc < result.min_ttc) result.min_ttc = target->ttc;
            result.approaching = 1;
        }
        if ((target->ttc >= 0.0f &&
             target->ttc < g_ttc_threshold) ||
            target->distance <= g_dist_threshold) {
            result.should_alert = 1;
        }

        float dist_ratio = target->distance /
                           fmaxf(g_dist_threshold, 0.1f);
        float ttc_ratio = (target->ttc >= 0.0f)
                              ? target->ttc / fmaxf(g_ttc_threshold, 0.1f)
                              : FLT_MAX;
        float risk = fminf(dist_ratio, ttc_ratio);
        if (risk < best_risk ||
            (fabsf(risk - best_risk) < 0.0001f &&
             result.dangerous_index >= 0 &&
             target->distance <
                 result.targets[result.dangerous_index].distance)) {
            best_risk = risk;
            result.dangerous_index = i;
            result.dangerous_obj_id = target->obj_id;
        }
    }

    return result;
}

/**
 * @brief 从雷达串口数据帧中提取 BSD 报告
 * @param frame     接收到的数据帧
 * @param frame_len 数据帧长度
 * @param out       输出解析结果
 * @return 1 成功解析到 BSD 报告，0 非 BSD 帧或无需处理，-1 数据异常
 *
 * 支持 HEAD_REPORT 报告帧和 HEAD_REPLY 回复帧，自动校验 sum8 校验和。
 */
static int process_radar_frame(const uint8_t *frame, int frame_len, radar_result_t *out) {
    if (frame_len < 4) return -1;
    uint8_t head = frame[0];
    if (head == HEAD_REPORT) {
        uint8_t len = frame[1];
        if (2 + len + 1 > frame_len) return -1;
        if (calc_sum8(frame, 2 + len) != frame[2 + len]) return -1;
        const uint8_t *payload = &frame[2];
        if (payload[0] != TYPE_BSD) return 0;
        /* LEN 包含 1 字节 TYPE；跳过 TYPE 后剩余长度必须同步减 1。 */
        int data_len = (int)len - 1;
        const uint8_t *data = payload + 1;
        if (data_len < 4) return 0;
        bsd_det_t bsd; memset(&bsd, 0, sizeof(bsd));
        bsd.obj_num = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
        int oc = bsd.obj_num; if (oc > MAX_RADAR_OBJECTS) oc = MAX_RADAR_OBJECTS;
        int expected = 4 + oc * (int)sizeof(bsd_obj_t);
        if (data_len < expected) oc = (data_len - 4) / (int)sizeof(bsd_obj_t);
        if (oc < 0) oc = 0;
        bsd.obj_num = (uint16_t)oc;
        for (int i = 0; i < oc; i++) {
            int off = 4 + i * (int)sizeof(bsd_obj_t);
            if (off + (int)sizeof(bsd_obj_t) <= data_len)
                memcpy(&bsd.obj[i], &data[off], sizeof(bsd_obj_t));
        }
        *out = process_bsd_report(&bsd);
        return 1;
    }
    return 0;
}

/* ======================== 雷达实验数据与 Dashboard 状态 ======================== */

static int mkdir_recursive(const char *path) {
    if (path == NULL || path[0] == '\0') return -1;
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp))
        return -1;

    size_t len = strlen(tmp);
    if (len > 1 && tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p != '\0'; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0775) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0775) != 0 && errno != EEXIST) return -1;
    return 0;
}

/*
 * 写入者在调用前必须先关闭文件。采用同目录 rename，避免复制大 CSV；
 * 当前文件加四份历史文件，总容量上限约为 100 MiB/日志类型。
 */
static int rotate_numbered_file(const char *path, int backups) {
    if (path == NULL || path[0] == '\0' || backups <= 0) return -1;

    char source[PATH_MAX];
    char destination[PATH_MAX];
    for (int index = backups; index >= 1; index--) {
        if (index == 1) {
            if (snprintf(source, sizeof(source), "%s", path) >=
                (int)sizeof(source))
                return -1;
        } else {
            if (snprintf(source, sizeof(source), "%s.%d", path, index - 1) >=
                (int)sizeof(source))
                return -1;
        }
        if (snprintf(destination, sizeof(destination), "%s.%d", path, index) >=
            (int)sizeof(destination))
            return -1;

        if (index == backups) unlink(destination);
        if (rename(source, destination) != 0 && errno != ENOENT) {
            fprintf(stderr, "[LOG_ROTATE] rename %s -> %s failed: %s\n",
                    source, destination, strerror(errno));
            return -1;
        }
    }
    return 0;
}

static FILE *open_csv_append(const char *path, const char *header) {
    struct stat st;
    int needs_header = (stat(path, &st) != 0 || st.st_size == 0);
    FILE *fp = fopen(path, "a");
    if (fp == NULL) return NULL;
    setvbuf(fp, NULL, _IOLBF, BUFSIZ);
    if (needs_header) {
        /* UTF-8 BOM 让 Excel/WPS 不再把 CSV 中文误判为 GBK/ANSI。 */
        fputs("\xEF\xBB\xBF", fp);
        fputs(header, fp);
    }
    return fp;
}

static int csv_needs_rotation(FILE *fp) {
    struct stat st;
    if (fp == NULL) return 0;
    if (fflush(fp) != 0 || fstat(fileno(fp), &st) != 0) return 0;
    return st.st_size >= (off_t)TELEMETRY_LOG_MAX_BYTES;
}

static void format_timestamp_iso(const struct timeval *tv,
                                 char *out, size_t out_size) {
    struct tm tm_utc;
    time_t seconds = tv->tv_sec;
    gmtime_r(&seconds, &tm_utc);
    char date[32];
    strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &tm_utc);
    int milliseconds = (int)(tv->tv_usec / 1000L);
    snprintf(out, out_size, "%s.%03dZ", date, milliseconds);
}

static void csv_sanitize(const char *input, char *output, size_t output_size) {
    if (output_size == 0) return;
    size_t used = 0;
    if (input != NULL) {
        for (const char *p = input; *p != '\0' && used + 1 < output_size; p++) {
            char ch = *p;
            if (ch == ',' || ch == '\r' || ch == '\n' || ch == '"') ch = ' ';
            output[used++] = ch;
        }
    }
    output[used] = '\0';
}

static const char SENSOR_CSV_HEADER[] =
    "timestamp,timestamp_ms,source,event_type,status,event_id,"
    "label,score,count,seq,reason,details\n";

static const char RADAR_CSV_HEADER[] =
    "timestamp,timestamp_ms,objId,distance_m,velocity_mps,"
    "angle_deg,filtered_angle_deg,TTC_s,direction,"
    "dangerous_objId,is_current_dangerous,radar_alert\n";

static void sensor_csv_maybe_rotate_locked(void) {
    g_sensor_csv_write_count++;
    if (g_sensor_csv == NULL ||
        g_sensor_csv_write_count % TELEMETRY_SIZE_CHECK_WRITES != 0 ||
        !csv_needs_rotation(g_sensor_csv))
        return;

    fclose(g_sensor_csv);
    g_sensor_csv = NULL;
    if (rotate_numbered_file(g_sensor_csv_path,
                             TELEMETRY_LOG_BACKUPS) != 0) {
        fprintf(stderr, "[SENSOR_DATA] Rotation failed for %s\n",
                g_sensor_csv_path);
    }
    g_sensor_csv = open_csv_append(g_sensor_csv_path, SENSOR_CSV_HEADER);
    g_sensor_csv_write_count = 0;
    if (g_sensor_csv == NULL) {
        fprintf(stderr, "[SENSOR_DATA] Reopen failed for %s: %s\n",
                g_sensor_csv_path, strerror(errno));
    } else {
        printf("[系统] [SENSOR_DATA] Rotated at %u MiB (keep=%d)\n",
               TELEMETRY_LOG_MAX_BYTES / (1024U * 1024U),
               TELEMETRY_LOG_BACKUPS);
    }
}

static int sensor_telemetry_init(void) {
    if (mkdir_recursive(g_radar_log_dir) != 0) return -1;
    if (snprintf(g_sensor_csv_path, sizeof(g_sensor_csv_path),
                 "%s/sensor_events.csv", g_radar_log_dir) >=
        (int)sizeof(g_sensor_csv_path))
        return -1;

    struct stat st;
    if (stat(g_sensor_csv_path, &st) == 0 &&
        st.st_size >= (off_t)TELEMETRY_LOG_MAX_BYTES)
        rotate_numbered_file(g_sensor_csv_path, TELEMETRY_LOG_BACKUPS);

    g_sensor_csv = open_csv_append(g_sensor_csv_path, SENSOR_CSV_HEADER);
    if (g_sensor_csv == NULL) {
        fprintf(stderr, "[SENSOR_DATA] Cannot open %s: %s\n",
                g_sensor_csv_path, strerror(errno));
        return -1;
    }
    g_sensor_csv_write_count = 0;
    printf("[系统] [SENSOR_DATA] CSV: %s (20 MiB x current+4)\n",
           g_sensor_csv_path);
    return 0;
}

static void sensor_event_log(const char *source, const char *event_type,
                             const char *status, const char *event_id,
                             const char *label, float score, int count,
                             int seq, const char *reason,
                             const char *details) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t timestamp_ms = (uint64_t)tv.tv_sec * 1000ULL +
                            (uint64_t)tv.tv_usec / 1000ULL;
    char timestamp[40], safe_source[32], safe_type[48], safe_status[32];
    char safe_id[96], safe_label[96], safe_reason[96], safe_details[512];
    format_timestamp_iso(&tv, timestamp, sizeof(timestamp));
    csv_sanitize(source, safe_source, sizeof(safe_source));
    csv_sanitize(event_type, safe_type, sizeof(safe_type));
    csv_sanitize(status, safe_status, sizeof(safe_status));
    csv_sanitize(event_id, safe_id, sizeof(safe_id));
    csv_sanitize(label, safe_label, sizeof(safe_label));
    csv_sanitize(reason, safe_reason, sizeof(safe_reason));
    csv_sanitize(details, safe_details, sizeof(safe_details));

    pthread_mutex_lock(&g_sensor_csv_mutex);
    if (g_sensor_csv == NULL) {
        pthread_mutex_unlock(&g_sensor_csv_mutex);
        return;
    }
    fprintf(g_sensor_csv, "%s,%llu,%s,%s,%s,%s,%s,",
            timestamp, (unsigned long long)timestamp_ms, safe_source,
            safe_type, safe_status, safe_id, safe_label);
    if (score >= 0.0f) fprintf(g_sensor_csv, "%.4f", score);
    fputc(',', g_sensor_csv);
    if (count >= 0) fprintf(g_sensor_csv, "%d", count);
    fputc(',', g_sensor_csv);
    if (seq >= 0) fprintf(g_sensor_csv, "%d", seq);
    fprintf(g_sensor_csv, ",%s,%s\n", safe_reason, safe_details);
    sensor_csv_maybe_rotate_locked();
    pthread_mutex_unlock(&g_sensor_csv_mutex);
}

static void sensor_telemetry_close(void) {
    pthread_mutex_lock(&g_sensor_csv_mutex);
    if (g_sensor_csv != NULL) {
        fclose(g_sensor_csv);
        g_sensor_csv = NULL;
    }
    pthread_mutex_unlock(&g_sensor_csv_mutex);
}

static int radar_telemetry_init(void) {
    if (mkdir_recursive(g_radar_log_dir) != 0) {
        fprintf(stderr, "[RADAR_DATA] Cannot create %s: %s\n",
                g_radar_log_dir, strerror(errno));
        return -1;
    }

    if (snprintf(g_radar_csv_path, sizeof(g_radar_csv_path),
                 "%s/radar_data.csv", g_radar_log_dir) >=
            (int)sizeof(g_radar_csv_path) ||
        snprintf(g_radar_state_path, sizeof(g_radar_state_path),
                 "%s/radar_state.json", g_radar_log_dir) >=
            (int)sizeof(g_radar_state_path)) {
        fprintf(stderr, "[RADAR_DATA] Log path is too long\n");
        return -1;
    }

    struct stat st;
    if (stat(g_radar_csv_path, &st) == 0 &&
        st.st_size >= (off_t)TELEMETRY_LOG_MAX_BYTES)
        rotate_numbered_file(g_radar_csv_path, TELEMETRY_LOG_BACKUPS);

    g_radar_csv = open_csv_append(g_radar_csv_path, RADAR_CSV_HEADER);
    if (g_radar_csv == NULL) {
        fprintf(stderr, "[RADAR_DATA] Cannot open %s: %s\n",
                g_radar_csv_path, strerror(errno));
        return -1;
    }
    g_radar_csv_write_count = 0;
    printf("[系统] [RADAR_DATA] CSV: %s (20 MiB x current+4)\n",
           g_radar_csv_path);
    printf("[系统] [RADAR_DATA] Dashboard state: %s\n",
           g_radar_state_path);
    return 0;
}

static void json_write_float_or_null(FILE *fp, float value) {
    if (isfinite(value) && value >= 0.0f)
        fprintf(fp, "%.3f", value);
    else
        fputs("null", fp);
}

static void radar_telemetry_publish(const radar_result_t *radar,
                                    int fusion_alert) {
    if (radar == NULL) return;

    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);
    uint64_t timestamp_ms = (uint64_t)tv_now.tv_sec * 1000ULL +
                            (uint64_t)tv_now.tv_usec / 1000ULL;
    g_radar_last_publish_ms = timestamp_ms;
    char timestamp[40];
    format_timestamp_iso(&tv_now, timestamp, sizeof(timestamp));

    if (g_radar_csv != NULL) {
        for (int i = 0; i < radar->obj_count; i++) {
            const radar_target_t *target = &radar->targets[i];
            fprintf(g_radar_csv,
                    "%s,%llu,%d,%.3f,%.3f,%.3f,%.3f,",
                    timestamp, (unsigned long long)timestamp_ms,
                    target->obj_id, target->distance, target->velocity,
                    target->angle, target->filtered_angle);
            if (target->ttc >= 0.0f)
                fprintf(g_radar_csv, "%.3f", target->ttc);
            fprintf(g_radar_csv, ",%s,%d,%d,%d\n",
                    radar_direction_name(target->direction),
                    radar->dangerous_obj_id,
                    i == radar->dangerous_index ? 1 : 0,
                    radar->should_alert ? 1 : 0);
        }
        g_radar_csv_write_count += (unsigned int)radar->obj_count;
        if (g_radar_csv_write_count % TELEMETRY_SIZE_CHECK_WRITES <
                (unsigned int)radar->obj_count &&
            csv_needs_rotation(g_radar_csv)) {
            fclose(g_radar_csv);
            g_radar_csv = NULL;
            if (rotate_numbered_file(g_radar_csv_path,
                                     TELEMETRY_LOG_BACKUPS) != 0) {
                fprintf(stderr, "[RADAR_DATA] Rotation failed for %s\n",
                        g_radar_csv_path);
            }
            g_radar_csv =
                open_csv_append(g_radar_csv_path, RADAR_CSV_HEADER);
            g_radar_csv_write_count = 0;
            if (g_radar_csv == NULL) {
                fprintf(stderr, "[RADAR_DATA] Reopen failed for %s: %s\n",
                        g_radar_csv_path, strerror(errno));
            } else {
                printf("[系统] [RADAR_DATA] Rotated at %u MiB (keep=%d)\n",
                       TELEMETRY_LOG_MAX_BYTES / (1024U * 1024U),
                       TELEMETRY_LOG_BACKUPS);
            }
        }
    }

    if (g_radar_state_path[0] == '\0') return;
    char tmp_path[PATH_MAX];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp",
                 g_radar_state_path) >= (int)sizeof(tmp_path))
        return;

    FILE *fp = fopen(tmp_path, "w");
    if (fp == NULL) return;

    fprintf(fp,
            "{\"timestamp\":\"%s\",\"timestamp_ms\":%llu,"
            "\"has_target\":%s,\"obj_count\":%d,"
            "\"dangerous_objId\":",
            timestamp, (unsigned long long)timestamp_ms,
            radar->has_target ? "true" : "false", radar->obj_count);
    if (radar->dangerous_index >= 0)
        fprintf(fp, "%d", radar->dangerous_obj_id);
    else
        fputs("null", fp);

    fputs(",\"distance_m\":", fp);
    if (radar->dangerous_index >= 0)
        fprintf(fp, "%.3f",
                radar->targets[radar->dangerous_index].distance);
    else
        fputs("null", fp);
    fputs(",\"velocity_mps\":", fp);
    if (radar->dangerous_index >= 0)
        fprintf(fp, "%.3f",
                radar->targets[radar->dangerous_index].velocity);
    else
        fputs("null", fp);
    fputs(",\"angle_deg\":", fp);
    if (radar->dangerous_index >= 0)
        fprintf(fp, "%.3f",
                radar->targets[radar->dangerous_index].angle);
    else
        fputs("null", fp);
    fputs(",\"filtered_angle_deg\":", fp);
    if (radar->dangerous_index >= 0)
        fprintf(fp, "%.3f",
                radar->targets[radar->dangerous_index].filtered_angle);
    else
        fputs("null", fp);
    fputs(",\"TTC_s\":", fp);
    if (radar->dangerous_index >= 0)
        json_write_float_or_null(
            fp, radar->targets[radar->dangerous_index].ttc);
    else
        fputs("null", fp);

    fprintf(fp,
            ",\"direction\":\"%s\",\"radar_alert\":%s,"
            "\"fusion_alert\":%s,"
            "\"thresholds\":{\"TTC_s\":%.3f,\"distance_m\":%.3f,"
            "\"left_angle_deg\":%.3f,\"right_angle_deg\":%.3f,"
            "\"angle_sign\":%.0f},"
            "\"targets\":[",
            radar->dangerous_index >= 0
                ? radar_direction_name(
                      radar->targets[radar->dangerous_index].direction)
                : "UNKNOWN",
            radar->should_alert ? "true" : "false",
            fusion_alert ? "true" : "false",
            g_ttc_threshold, g_dist_threshold,
            g_angle_left_threshold, g_angle_right_threshold,
            g_angle_direction_sign);

    for (int i = 0; i < radar->obj_count; i++) {
        const radar_target_t *target = &radar->targets[i];
        if (i > 0) fputc(',', fp);
        fprintf(fp,
                "{\"objId\":%d,\"distance_m\":%.3f,"
                "\"velocity_mps\":%.3f,\"angle_deg\":%.3f,"
                "\"filtered_angle_deg\":%.3f,\"TTC_s\":",
                target->obj_id, target->distance, target->velocity,
                target->angle, target->filtered_angle);
        json_write_float_or_null(fp, target->ttc);
        fprintf(fp,
                ",\"direction\":\"%s\",\"is_dangerous\":%s}",
                radar_direction_name(target->direction),
                i == radar->dangerous_index ? "true" : "false");
    }
    fputs("]}\n", fp);
    if (fclose(fp) == 0) {
        if (rename(tmp_path, g_radar_state_path) != 0)
            unlink(tmp_path);
    } else {
        unlink(tmp_path);
    }
}

static void radar_telemetry_publish_empty(void) {
    radar_result_t empty = {0};
    empty.dangerous_index = -1;
    empty.dangerous_obj_id = -1;
    radar_telemetry_publish(&empty, 0);
}

static void radar_telemetry_close(void) {
    if (g_radar_csv != NULL) {
        fclose(g_radar_csv);
        g_radar_csv = NULL;
    }
}

/* ======================== 雷达初始化 ======================== */
static int radar_init(int fd) {
    printf("[RADAR] Initializing...\n"); fflush(stdout);
    uint8_t rx_buf[512];
    auto wait_reply = [&](int timeout_ms) {
        struct timeval start, now;
        gettimeofday(&start, NULL);
        while (g_running) {
            gettimeofday(&now, NULL);
            int elapsed = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_usec - start.tv_usec) / 1000;
            if (elapsed >= timeout_ms) break;
            int remain = timeout_ms - elapsed;
            struct timeval tv = { remain / 1000, (remain % 1000) * 1000 };
            fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
            if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) break;
            (void)read(fd, rx_buf, sizeof(rx_buf));
        }
    };
    flush_rx(fd);
    send_cmd(fd, 7, 0x1E, NULL, 0); wait_reply(1000); usleep(200000);
    flush_rx(fd);
    { uint8_t p = 0x01; send_cmd(fd, 6, 0x11, &p, 1); } wait_reply(1000); usleep(200000);
    flush_rx(fd);
    { uint8_t p = 0x00; send_cmd(fd, 0, 0x02, &p, 1); } wait_reply(1000); usleep(200000);
    flush_rx(fd);
    { uint8_t p[2] = {0x88, 0x13}; send_cmd(fd, 6, 0x12, p, 2); } wait_reply(1000); usleep(200000);
    printf("[RADAR] Initialized OK\n");
    return 0;
}

/* ======================== DVR 模块 ======================== */
static int dvr_recording = 0;
static int dvr_save_triggered = 0;
static int dvr_encoding = 0;
static pid_t dvr_encoder_pid = 0;
static uint64_t dvr_trigger_time_us = 0;
static int dvr_frame_count = 0;
static dvr_frame_entry_t dvr_frames[DVR_MAX_FRAMES];
static FILE *dvr_raw_file = NULL;
static char dvr_raw_path[2048];
static char dvr_buffer_dir[1024];
static int dvr_tf_ok = 0;
static int dvr_has_encoder = 0;
static int dvr_camera_pixelformat = 0;
static uint64_t dvr_last_start_attempt_us = 0;
static uint64_t dvr_last_forced_start_attempt_us = 0;
#define DVR_START_RETRY_US 2000000ULL

/* 生成输出文件名 */
static void dvr_make_filename(char *buf, size_t bufsz, const char *prefix) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    snprintf(buf, bufsz, "%s/%s_%04d%02d%02d_%02d%02d%02d.mp4",
             DVR_BASE_DIR, prefix,
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
}

/* 启动 DVR 录制 */
static int dvr_start(void) {
    if (dvr_recording) return 0;
    struct stat storage_st;
    if (!dvr_tf_ok &&
        stat(DVR_BASE_DIR, &storage_st) == 0 &&
        S_ISDIR(storage_st.st_mode)) {
        dvr_tf_ok = 1;
    }
    if (!dvr_tf_ok) {
        printf("[系统] [DVR] TF card not available, recording disabled\n");
        return -1;
    }

    /* 创建目录 */
    snprintf(dvr_buffer_dir, sizeof(dvr_buffer_dir), "%s", DVR_BUFFER_DIR);
    if (mkdir(DVR_BASE_DIR, 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "[DVR] Cannot create base directory %s: %s\n",
                DVR_BASE_DIR, strerror(errno));
        return -1;
    }
    if (mkdir(dvr_buffer_dir, 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "[DVR] Cannot create buffer directory %s: %s\n",
                dvr_buffer_dir, strerror(errno));
        return -1;
    }

    /* 打开原始帧文件 */
    snprintf(dvr_raw_path, sizeof(dvr_raw_path), "%s/dvr_raw.bin", dvr_buffer_dir);
    dvr_raw_file = fopen(dvr_raw_path, "wb");
    if (!dvr_raw_file) {
        fprintf(stderr, "[DVR] Cannot create raw file: %s\n", strerror(errno));
        return -1;
    }

    dvr_frame_count = 0;
    dvr_save_triggered = 0;
    dvr_encoding = 0;
    dvr_recording = 1;
    printf("[调试] [DVR] Recording started (buffer: %s)\n", dvr_buffer_dir);
    return 0;
}

/*
 * NPU 首次发现目标时启动预缓存；如果当时 TF/目录短暂不可用，后续仍要重试。
 * 碰撞和摔倒事件使用 force=1 立即再试，避免 dvr_recording=0 时静默丢失视频。
 */
static int dvr_ensure_started(uint64_t now_us, const char *reason, int force) {
    if (dvr_recording) return 0;
    if (dvr_encoding) return -1;
    uint64_t retry_us = force ? 500000ULL : DVR_START_RETRY_US;
    uint64_t last_attempt_us =
        force ? dvr_last_forced_start_attempt_us : dvr_last_start_attempt_us;
    if (last_attempt_us != 0 &&
        now_us >= last_attempt_us &&
        now_us - last_attempt_us < retry_us)
        return -1;

    dvr_last_start_attempt_us = now_us;
    if (force) dvr_last_forced_start_attempt_us = now_us;
    int rc = dvr_start();
    char details[192];
    snprintf(details, sizeof(details),
             "reason=%s recording=%d encoding=%d tf_ok=%d rc=%d",
             reason != NULL ? reason : "unknown", dvr_recording,
             dvr_encoding, dvr_tf_ok, rc);
    sensor_event_log("a35_dvr", "buffer",
                     rc == 0 ? "started" : "failed",
                     NULL, NULL, -1.0f, -1, -1,
                     reason, details);
    return rc;
}

/* 保存一帧到 DVR 缓冲 */
static void dvr_save_frame(const uint8_t *jpeg_data, uint32_t jpeg_size, uint64_t timestamp_us) {
    if (!dvr_recording || dvr_encoding || !dvr_raw_file) return;

    /* 只支持 MJPEG 格式; 其他格式需要转码, 这里跳过避免写入错误数据 */
    if (dvr_camera_pixelformat != V4L2_PIX_FMT_MJPEG) {
        static int warned = 0;
        if (!warned) {
            printf("[调试] [DVR] Camera format is not MJPEG, skipping DVR frames\n");
            warned = 1;
        }
        return;
    }

    /* 校验 JPEG 边界: 必须以 SOI (FFD8) 开始, EOI (FFD9) 结束
     * 部分摄像头会在 EOI 后补 0x00, 所以搜索最后 16 字节内的 FFD9 */
    int has_eoi = 0;
    if (jpeg_size >= 4 && jpeg_data[0] == 0xFF && jpeg_data[1] == 0xD8) {
        int scan_start = (jpeg_size > 16) ? (jpeg_size - 16) : 0;
        for (int i = jpeg_size - 2; i >= scan_start; i--) {
            if (jpeg_data[i] == 0xFF && jpeg_data[i + 1] == 0xD9) {
                has_eoi = 1;
                break;
            }
        }
    }
    if (!has_eoi) {
        static int warned = 0;
        if (!warned) {
            printf("[调试] [DVR] Invalid JPEG frame skipped (size=%u, head=%02X%02X tail=%02X%02X)\n",
                   jpeg_size,
                   jpeg_size > 0 ? jpeg_data[0] : 0, jpeg_size > 1 ? jpeg_data[1] : 0,
                   jpeg_size > 2 ? jpeg_data[jpeg_size - 2] : 0, jpeg_size > 1 ? jpeg_data[jpeg_size - 1] : 0);
            warned = 1;
        }
        return;
    }

    /* 记录帧信息 */
    if (dvr_frame_count < DVR_MAX_FRAMES) {
        dvr_frames[dvr_frame_count].timestamp_us = timestamp_us;
        dvr_frames[dvr_frame_count].file_offset = ftello(dvr_raw_file);
        dvr_frames[dvr_frame_count].jpeg_size = jpeg_size;
        dvr_frames[dvr_frame_count].frame_index = dvr_frame_count;
    } else {
        /* 环形覆盖: 移动数组 */
        memmove(dvr_frames, dvr_frames + 1, (DVR_MAX_FRAMES - 1) * sizeof(dvr_frame_entry_t));
        dvr_frames[DVR_MAX_FRAMES - 1].timestamp_us = timestamp_us;
        dvr_frames[DVR_MAX_FRAMES - 1].file_offset = ftello(dvr_raw_file);
        dvr_frames[DVR_MAX_FRAMES - 1].jpeg_size = jpeg_size;
        dvr_frames[DVR_MAX_FRAMES - 1].frame_index = dvr_frame_count;
    }

    /* 写入帧: [4B size][8B timestamp][JPEG data] */
    uint32_t frame_size = jpeg_size + 12;
    fwrite(&frame_size, 4, 1, dvr_raw_file);
    fwrite(&timestamp_us, 8, 1, dvr_raw_file);
    fwrite(jpeg_data, 1, jpeg_size, dvr_raw_file);

    /* 每 25 帧 flush 一次，避免每帧都 sync 磁盘 */
    static int dvr_flush_cnt = 0;
    if (++dvr_flush_cnt >= DVR_CAPTURE_FPS) {
        fflush(dvr_raw_file);
        dvr_flush_cnt = 0;
    }

    dvr_frame_count++;
}

/* 触发 DVR 保存 */
static void dvr_trigger_save(uint64_t trigger_time_us, const char *reason) {
    if (!dvr_recording || dvr_save_triggered) return;
    /* 先写时间再置标志，避免主循环看到标志却读到旧时间 */
    dvr_trigger_time_us = trigger_time_us;
    dvr_save_triggered = 1;
    printf("[保存] [DVR] Save triggered! (pre=%ds, post=%ds)\n",
           DVR_SAVE_BEFORE_SEC, DVR_SAVE_AFTER_SEC);
    char details[160];
    snprintf(details, sizeof(details),
             "reason=%s frames=%d pre_s=%d post_s=%d",
             reason != NULL ? reason : "unknown", dvr_frame_count,
             DVR_SAVE_BEFORE_SEC, DVR_SAVE_AFTER_SEC);
    sensor_event_log("a35_dvr", "recording", "triggered",
                     NULL, NULL, -1.0f, -1, -1,
                     reason, details);
}

/* 停止 DVR 录制 */
static void dvr_stop(void) {
    if (!dvr_recording) return;
    dvr_recording = 0;

    if (dvr_raw_file) {
        fclose(dvr_raw_file);
        dvr_raw_file = NULL;
    }

    if (!dvr_save_triggered) {
        /* 无触发: 清理缓冲 */
        printf("[调试] [DVR] No trigger event, cleaning up buffer\n");
        unlink(dvr_raw_path);
        rmdir(dvr_buffer_dir);
    }
}

/* 在子进程内运行一个编码器命令, 等待结束并返回退出码。
 * 输出重定向到 log_path。
 */
static int run_encoder_in_child(const char *name, char *const argv[], const char *log_path) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }
        execvp(name, argv);
        _exit(127); /* 命令不存在 */
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

/* 录制完成提示音 */
static void play_recording_complete_sound(void) {
    const char *file = AUDIO_RECORDING_COMPLETE;
    if (access(file, F_OK) != 0) {
        printf("[AUDIO] Recording complete sound not found: %s\n", file);
        return;
    }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "aplay -q %s >/dev/null 2>&1 &", file);
    if (system(cmd) == -1) {
        fprintf(stderr, "[AUDIO] Failed to play recording complete sound\n");
    } else {
        printf("[保存] [AUDIO] Playing recording complete notification\n");
    }
}

/* 子进程: 将缓冲帧编码为 MP4 (异步, 不阻塞主循环) */
static int dvr_encode_mp4(void) {
    if (dvr_encoding) return -1;
    if (dvr_frame_count == 0) return -1;
    if (!dvr_has_encoder) {
        printf("[系统] [DVR] No video encoder available, skipping encode\n");
        /* 清理缓冲 */
        unlink(dvr_raw_path);
        rmdir(dvr_buffer_dir);
        return -1;
    }

    dvr_encoding = 1;

    /* 找触发前 15 秒的起始帧 */
    int active_frames = dvr_frame_count < DVR_MAX_FRAMES ? dvr_frame_count : DVR_MAX_FRAMES;
    int start_idx = 0;
    int end_idx = active_frames - 1;

    if (dvr_save_triggered && dvr_trigger_time_us > 0) {
        uint64_t before_us = dvr_trigger_time_us - (uint64_t)DVR_SAVE_BEFORE_SEC * 1000000ULL;
        for (int i = 0; i < active_frames; i++) {
            if (dvr_frames[i].timestamp_us >= before_us) {
                start_idx = i;
                break;
            }
        }
        uint64_t after_us = dvr_trigger_time_us + (uint64_t)DVR_SAVE_AFTER_SEC * 1000000ULL;
        for (int i = active_frames - 1; i >= 0; i--) {
            if (dvr_frames[i].timestamp_us <= after_us) {
                end_idx = i;
                break;
            }
        }
    }

    int total = end_idx - start_idx + 1;
    if (total < 2) {
        printf("[保存] [DVR] Too few frames (%d), skipping encode\n", total);
        dvr_encoding = 0;
        return -1;
    }

    printf("[保存] [DVR] Encoding %d frames (%d→%d) to MP4 (async)...\n", total, start_idx, end_idx);

    /* 计算帧率 */
    float fps = (float)DVR_CAPTURE_FPS;
    if (total >= 2) {
        uint64_t duration_us = dvr_frames[end_idx].timestamp_us - dvr_frames[start_idx].timestamp_us;
        if (duration_us > 0)
            fps = (float)(total - 1) * 1000000.0f / (float)duration_us;
    }
    if (fps < 1) fps = 1;
    if (fps > 30) fps = 30;

    printf("[保存] [DVR] FPS: %.1f, Frames: %d\n", fps, total);

    /* 生成输出文件名 */
    char output_path[1024];
    dvr_make_filename(output_path, sizeof(output_path), "emergency");

    /* 复制必要数据到堆上 (fork 后子进程使用) */
    char *heap_raw_path = strdup(dvr_raw_path);
    char *heap_buffer_dir = strdup(dvr_buffer_dir);
    char *heap_output_path = strdup(output_path);
    float heap_fps = fps;
    int heap_total = total;
    int heap_start_idx = start_idx;

    /* fork 前刷新缓冲区，避免日志被重复写入子进程 */
    fflush(NULL);

    /* fork 子进程做编码, 父进程立即返回 */
    pid_t pid = fork();
    if (pid == 0) {
        /* ========== 子进程: 编码 + 清理 ==========
         * 子进程忽略 SIGTERM, 防止父进程被 timeout 等工具终止时
         * 打断 ffmpeg 编码。
         */
        signal(SIGTERM, SIG_IGN);
        setsid();
        char work_dir[1024];
        snprintf(work_dir, sizeof(work_dir), "%s/.encode", heap_buffer_dir);
        mkdir(work_dir, 0777);

        char list_path[1024];
        snprintf(list_path, sizeof(list_path), "%s/filelist.txt", heap_buffer_dir);

        FILE *raw = fopen(heap_raw_path, "rb");
        if (!raw) {
            fprintf(stderr, "[DVR] Child: cannot open raw file\n");
            free(heap_raw_path); free(heap_buffer_dir); free(heap_output_path);
            exit(1);
        }

        FILE *list = fopen(list_path, "w");
        if (!list) { fclose(raw); free(heap_raw_path); free(heap_buffer_dir); free(heap_output_path); exit(1); }

        int frame_idx = 0;
        for (int i = heap_start_idx; i <= heap_start_idx + heap_total - 1; i++) {
            char jpg_path[1024];
            snprintf(jpg_path, sizeof(jpg_path), "%s/frame_%06d.jpg", work_dir, frame_idx);

            fseeko(raw, dvr_frames[i].file_offset, SEEK_SET);
            uint32_t frame_size;
            uint64_t ts;
            if (fread(&frame_size, 4, 1, raw) != 1) break;
            if (fread(&ts, 8, 1, raw) != 1) break;
            uint32_t jpeg_size = frame_size - 12;

            uint8_t *jpeg_data = (uint8_t *)malloc(jpeg_size);
            if (!jpeg_data) break;
            if (fread(jpeg_data, 1, jpeg_size, raw) != jpeg_size) {
                free(jpeg_data);
                break;
            }

            FILE *jpg = fopen(jpg_path, "wb");
            if (jpg) {
                fwrite(jpeg_data, 1, jpeg_size, jpg);
                fclose(jpg);
                fprintf(list, "file '%s'\n", jpg_path);
            }
            free(jpeg_data);
            frame_idx++;
        }
        fclose(list);
        fclose(raw);

        printf("[保存] [DVR] Child: extracted %d frames, running encoder (fps=%.1f)...\n", frame_idx, heap_fps);

        char fps_str[32];
        snprintf(fps_str, sizeof(fps_str), "%.2f", heap_fps);

        /* GStreamer 的 caps 需要整数帧率，例如 25/1 */
        int gst_fps_i = (int)(heap_fps + 0.5f);
        if (gst_fps_i < 1) gst_fps_i = 1;
        char gst_fps_str[32];
        snprintf(gst_fps_str, sizeof(gst_fps_str), "%d/1", gst_fps_i);

        char ffmpeg_log[1024];
        snprintf(ffmpeg_log, sizeof(ffmpeg_log), "%s/dvr_ffmpeg.log", heap_buffer_dir);

        char gst_location_arg[1024];
        char gst_caps_arg[1024];
        char gst_sink_arg[1024];
        snprintf(gst_location_arg, sizeof(gst_location_arg), "location=%s/frame_%%06d.jpg", work_dir);
        snprintf(gst_caps_arg, sizeof(gst_caps_arg), "caps=image/jpeg,framerate=%s", gst_fps_str);
        snprintf(gst_sink_arg, sizeof(gst_sink_arg), "location=%s", heap_output_path);

        int encoder_ok = 0;

        /* 1) GStreamer: JPEG 序列 -> v4l2slh264enc -> MP4 (H.264，兼容性最好) */
        {
            char *argv[] = {
                (char *)"gst-launch-1.0",
                (char *)"-e",
                (char *)"multifilesrc", gst_location_arg, (char *)"start-index=0", gst_caps_arg,
                (char *)"!", (char *)"jpegdec",
                (char *)"!", (char *)"videoconvert",
                (char *)"!", (char *)"video/x-raw,format=NV12",
                (char *)"!", (char *)"v4l2slh264enc", (char *)"bitrate=4000000",
                (char *)"!", (char *)"h264parse",
                (char *)"!", (char *)"mp4mux",
                (char *)"!", (char *)"filesink", gst_sink_arg,
                NULL
            };
            int rc = run_encoder_in_child("gst-launch-1.0", argv, ffmpeg_log);
            struct stat st;
            if (rc == 0 && stat(heap_output_path, &st) == 0 && st.st_size > 0) {
                printf("[保存] [DVR] Child: Saved %s (gst-launch MP4 H.264)\n", heap_output_path);
                encoder_ok = 1;
            } else {
                printf("[保存] [DVR] Child: gst-launch failed (rc=%d), trying ffmpeg, log=%s\n", rc, ffmpeg_log);
            }
        }

        /* 2) gst-launch 失败/输出为空, 回退到 ffmpeg mpeg4 */
        if (!encoder_ok) {
            char *argv[] = {
                (char *)"ffmpeg",
                (char *)"-hide_banner", (char *)"-loglevel", (char *)"warning",
                (char *)"-framerate", fps_str,
                (char *)"-i", list_path,
                (char *)"-c:v", (char *)"mpeg4", (char *)"-q:v", (char *)"5",
                (char *)"-pix_fmt", (char *)"yuv420p",
                (char *)"-y", heap_output_path,
                NULL
            };
            int rc = run_encoder_in_child("ffmpeg", argv, ffmpeg_log);
            struct stat st;
            if (rc == 0 && stat(heap_output_path, &st) == 0 && st.st_size > 0) {
                printf("[保存] [DVR] Child: Saved %s (ffmpeg mpeg4)\n", heap_output_path);
                encoder_ok = 1;
            } else {
                printf("[保存] [DVR] Child: ffmpeg failed (rc=%d), trying avconv, log=%s\n", rc, ffmpeg_log);
            }
        }

        /* 3) ffmpeg 不可用/失败, 尝试 avconv */
        if (!encoder_ok) {
            char *argv[] = {
                (char *)"avconv",
                (char *)"-hide_banner", (char *)"-loglevel", (char *)"warning",
                (char *)"-framerate", fps_str,
                (char *)"-i", list_path,
                (char *)"-c:v", (char *)"mpeg4", (char *)"-q:v", (char *)"5",
                (char *)"-pix_fmt", (char *)"yuv420p",
                (char *)"-y", heap_output_path,
                NULL
            };
            int rc = run_encoder_in_child("avconv", argv, ffmpeg_log);
            struct stat st;
            if (rc == 0 && stat(heap_output_path, &st) == 0 && st.st_size > 0) {
                printf("[保存] [DVR] Child: Saved %s (avconv mpeg4)\n", heap_output_path);
                encoder_ok = 1;
            } else {
                printf("[保存] [DVR] Child: avconv failed (rc=%d), log=%s\n", rc, ffmpeg_log);
            }
        }

        if (encoder_ok) {
            /* 成功后清理临时文件 */
            unlink(list_path);
            DIR *d = opendir(work_dir);
            if (d) {
                struct dirent *ent;
                char path[1024];
                while ((ent = readdir(d)) != NULL) {
                    if (ent->d_name[0] == '.') continue;
                    snprintf(path, sizeof(path), "%s/%s", work_dir, ent->d_name);
                    unlink(path);
                }
                closedir(d);
            }
            rmdir(work_dir);
            unlink(ffmpeg_log);
            play_recording_complete_sound();
        } else {
            printf("[保存] [DVR] Child: All encoders failed, see %s\n", ffmpeg_log);
            /* 保留 jpg 文件作为备份 */
        }

        /* 清理原始缓冲 */
        unlink(heap_raw_path);
        rmdir(heap_buffer_dir);

        free(heap_raw_path);
        free(heap_buffer_dir);
        free(heap_output_path);

        printf("[保存] [DVR] Child: cleanup done, exiting\n");
        exit(0);
    }

    /* ========== 父进程: 记录 pid, 立即返回 ========== */
    free(heap_raw_path);
    free(heap_buffer_dir);
    free(heap_output_path);

    if (pid > 0) {
        dvr_encoder_pid = pid;
        printf("[保存] [DVR] Async encoding started (pid=%d)\n", pid);
    } else {
        fprintf(stderr, "[DVR] fork failed: %s\n", strerror(errno));
        dvr_encoding = 0;
        return -1;
    }

    return 0;
}

/* ======================== 音频提示 ======================== */
static void play_alert_sound(const char *type) {
    const char *file = NULL;
    if (strcmp(type, "fall") == 0)       file = AUDIO_FALL;
    else if (strcmp(type, "collision") == 0) file = AUDIO_COLLISION;
    else                                 return;

    if (access(file, F_OK) != 0) {
        printf("[AUDIO] Sound file not found: %s\n", file);
        return;
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "aplay -q %s >/dev/null 2>&1 &", file);
    if (system(cmd) == -1) {
        fprintf(stderr, "[AUDIO] Failed to play %s\n", file);
    } else {
        printf("[告警] [AUDIO] Playing %s alert\n", type);
    }
}

static void play_v2x_alert(const char *direction) {
    const char *file = NULL;
    if (strcmp(direction, "nearby") == 0)       file = AUDIO_V2X_NEARBY;
    else if (strcmp(direction, "left_front") == 0)  file = AUDIO_V2X_LEFT_FRONT;
    else if (strcmp(direction, "right_front") == 0) file = AUDIO_V2X_RIGHT_FRONT;
    else if (strcmp(direction, "left") == 0)      file = AUDIO_V2X_LEFT;
    else if (strcmp(direction, "right") == 0)     file = AUDIO_V2X_RIGHT;
    else                                          file = AUDIO_V2X_NEARBY;

    if (access(file, F_OK) != 0) {
        printf("[AUDIO] V2X sound file not found: %s\n", file);
        return;
    }

    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);
    uint64_t now_us = (uint64_t)(tv_now.tv_sec - g_t_start.tv_sec) * 1000000ULL +
                      (uint64_t)tv_now.tv_usec;
    if (g_last_v2x_audio_us != 0 && (now_us - g_last_v2x_audio_us) < V2X_AUDIO_COOLDOWN_US) {
        printf("[V2X] Audio cooldown, skip playing (%s)\n", direction);
        return;
    }
    g_last_v2x_audio_us = now_us;

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "aplay -q %s >/dev/null 2>&1 &", file);
    if (system(cmd) == -1) {
        fprintf(stderr, "[AUDIO] Failed to play v2x %s alert\n", direction);
    } else {
        printf("[告警] [AUDIO] Playing V2X alert: %s\n", direction);
    }
}

static void handle_v2x_alert(const char *direction) {
    if (!direction) direction = "nearby";
    printf("[告警] [V2X] V2X_ALERT direction=%s\n", direction);
    g_v2x_alert = 1;
    /* V2X 只走音频提示，不驱动 LED；LED 留给雷达碰撞/IMU 摔倒 */
    play_v2x_alert(direction);
}

/* ======================== IMU 摔倒触发处理 ======================== */
static void handle_fall_trigger(uint64_t trigger_time_us) {
    if (g_imu_fall_alert) return;  /* 已触发, 忽略重复 */
    g_imu_fall_alert = 1;
    g_imu_fall_time_us = trigger_time_us;

    printf("[告警] [IMU] FALL DETECTED! Triggering emergency save\n");
    play_alert_sound("fall");

    /* 若摄像头可用但未在缓冲, 立即启动缓冲 (从摔倒瞬间开始) */
    if (g_camera_ok_global && !dvr_recording && !dvr_encoding) {
        if (dvr_ensure_started(trigger_time_us, "fall", 1) == 0) {
            printf("[保存] [DVR] Fall-triggered recording started (no pre-buffer)\n");
        }
    }

    /* 触发保存 (若已在缓冲则保留 pre 15s, 否则从当前开始) */
    if (dvr_recording && !dvr_save_triggered && !dvr_encoding) {
        dvr_trigger_save(trigger_time_us, "fall");
    }
}

/* ======================== IMU 异常事件 UDP 转发到 HUD/App ======================== */
/**
 * @brief 发送 JSON 到 HUD 输入端口（HUD 再转发到手机 App）
 * @param json 要发送的 JSON 字符串
 *
 * 与 v2x_alert_link.sh 行为一致：UDP 127.0.0.1:8890 -> HUD -> App
 */
static int udp_send_to_hud(const char *json) {
    if (json == NULL || json[0] == '\0') return -1;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "[IMU_FWD] socket failed: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(HUD_INPUT_PORT);
    if (inet_pton(AF_INET, HUD_INPUT_IP, &addr.sin_addr) <= 0) {
        fprintf(stderr, "[IMU_FWD] inet_pton failed: %s\n", strerror(errno));
        close(sock);
        return -1;
    }

    ssize_t sent = sendto(sock, json, strlen(json), 0,
                          (struct sockaddr *)&addr, sizeof(addr));
    if (sent < 0) {
        fprintf(stderr, "[IMU_FWD] sendto failed: %s\n", strerror(errno));
    } else {
        printf("[系统] [IMU_FWD] Forwarded %zd bytes to %s:%d\n",
               sent, HUD_INPUT_IP, HUD_INPUT_PORT);
    }
    close(sock);
    return sent >= 0 ? (int)sent : -1;
}

/**
 * @brief 从 IMU_ALERT 行中提取 key=value 形式的整数值
 */
static int parse_kv_int(const char *line, const char *key) {
    if (line == NULL || key == NULL) return 0;
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "%s=", key);
    const char *start = strstr(line, pattern);
    if (start == NULL) return 0;
    start += strlen(pattern);
    char *end = NULL;
    long val = strtol(start, &end, 10);
    if (end == start) return 0;
    return (int)val;
}

/**
 * @brief 解析并转发 IMU 异常事件到 HUD
 * @param line M33 输出的 IMU_ALERT 行
 *
 * 支持 type=fall / hard_brake / road_bump，与 v2x_alert_link.sh 映射一致。
 * 同时保留原有的摔倒触发逻辑（音频 + DVR）。
 */
static void forward_imu_alert(const char *line) {
    if (line == NULL || strstr(line, "IMU_ALERT") == NULL) return;

    const char *type_start = strstr(line, "type=");
    if (type_start == NULL) return;
    type_start += strlen("type=");

    const char *app_type = NULL;
    const char *message = NULL;
    char m33_type[32] = {0};
    if (strncmp(type_start, "fall", 4) == 0) {
        app_type = "fall_down";
        message = "多用户在此摔倒，请减速慢行";
        snprintf(m33_type, sizeof(m33_type), "fall");
    } else if (strncmp(type_start, "hard_brake", 10) == 0) {
        app_type = "emergency_brake";
        message = "多用户急刹，请注意减速";
        snprintf(m33_type, sizeof(m33_type), "hard_brake");
    } else if (strncmp(type_start, "road_bump", 9) == 0) {
        app_type = "road_hazard";
        message = "前方路面颠簸，请注意避让";
        snprintf(m33_type, sizeof(m33_type), "road_bump");
    } else {
        printf("[IMU_FWD] Unknown IMU type, skip forwarding\n");
        return;
    }

    int seq           = parse_kv_int(line, "seq");
    int gps_valid     = parse_kv_int(line, "gps_valid");
    int lat_1e7       = parse_kv_int(line, "lat_1e7");
    int lon_1e7       = parse_kv_int(line, "lon_1e7");
    int speed_cms     = parse_kv_int(line, "speed_cms");
    int heading_cdeg  = parse_kv_int(line, "heading_cdeg");
    int tick           = parse_kv_int(line, "tick");
    int acc_norm       = parse_kv_int(line, "acc_norm");
    int horiz_acc      = parse_kv_int(line, "horiz_acc");
    int z_delta        = parse_kv_int(line, "z_delta");
    int brake_y_delta  = parse_kv_int(line, "brake_y_delta");
    int ax             = parse_kv_int(line, "ax");
    int ay             = parse_kv_int(line, "ay");
    int az             = parse_kv_int(line, "az");
    int gx             = parse_kv_int(line, "gx");
    int gy             = parse_kv_int(line, "gy");
    int gz             = parse_kv_int(line, "gz");
    int roll           = parse_kv_int(line, "roll");
    int pitch          = parse_kv_int(line, "pitch");
    int yaw            = parse_kv_int(line, "yaw");
    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);
    uint64_t timestamp_ms = (uint64_t)tv_now.tv_sec * 1000ULL +
                            (uint64_t)tv_now.tv_usec / 1000ULL;

    const char *reason = strstr(line, "reason=");
    char reason_buf[64] = "unknown";
    if (reason != NULL) {
        reason += strlen("reason=");
        const char *reason_end = strchr(reason, ' ');
        int len = (reason_end != NULL) ? (int)(reason_end - reason) : (int)strlen(reason);
        if (len > 0 && len < (int)sizeof(reason_buf)) {
            memcpy(reason_buf, reason, len);
            reason_buf[len] = '\0';
        }
    }

    char event_id[96];
    snprintf(event_id, sizeof(event_id), "m33-%d-%llu", seq,
             (unsigned long long)timestamp_ms);
    char details[512];
    snprintf(details, sizeof(details),
             "tick=%d acc_norm=%d horiz_acc=%d z_delta=%d brake_y_delta=%d "
             "ax=%d ay=%d az=%d gx=%d gy=%d gz=%d roll=%d pitch=%d yaw=%d",
             tick, acc_norm, horiz_acc, z_delta, brake_y_delta,
             ax, ay, az, gx, gy, gz, roll, pitch, yaw);
    sensor_event_log("imu_m33", m33_type, "received", event_id, NULL,
                     -1.0f, -1, seq, reason_buf, details);

    char json[1536];
    snprintf(json, sizeof(json),
             "{\"type\":\"%s\",\"message\":\"%s\",\"source\":\"M33_A35\","
             "\"m33_type\":\"%s\",\"reason\":\"%s\",\"seq\":%d,"
             "\"event_id\":\"%s\",\"a35_timestamp_ms\":%llu,"
             "\"requires_sms\":%s,\"tick\":%d,\"acc_norm\":%d,"
             "\"horiz_acc\":%d,\"z_delta\":%d,\"brake_y_delta\":%d,"
             "\"ax\":%d,\"ay\":%d,\"az\":%d,\"gx\":%d,\"gy\":%d,"
             "\"gz\":%d,\"roll\":%d,\"pitch\":%d,\"yaw\":%d,"
             "\"gps_valid\":%d,\"lat_1e7\":%d,\"lon_1e7\":%d,"
             "\"speed_cms\":%d,\"heading_cdeg\":%d}",
             app_type, message, m33_type, reason_buf, seq,
             event_id, (unsigned long long)timestamp_ms,
             strcmp(m33_type, "fall") == 0 ? "true" : "false",
             tick, acc_norm, horiz_acc, z_delta, brake_y_delta,
             ax, ay, az, gx, gy, gz, roll, pitch, yaw,
             gps_valid, lat_1e7, lon_1e7, speed_cms, heading_cdeg);

    printf("[系统] [IMU_FWD] Forwarding IMU event: type=%s\n", app_type);
    int bytes = udp_send_to_hud(json);
    char send_details[96];
    snprintf(send_details, sizeof(send_details), "udp_bytes=%d hud_port=%d",
             bytes, HUD_INPUT_PORT);
    sensor_event_log("a35_hud", app_type,
                     bytes >= 0 ? "sent" : "failed", event_id, NULL,
                     -1.0f, -1, seq, reason_buf, send_details);
}

/* ======================== 测试模式: 模拟 IMU 摔倒触发 ======================== */
static int g_test_fall_delay_sec = 0;
static void *test_fall_thread(void *arg) {
    (void)arg;
    if (g_test_fall_delay_sec <= 0) return NULL;
    printf("[TEST] Simulating IMU fall after %d seconds...\n", g_test_fall_delay_sec);
    for (int i = 0; i < g_test_fall_delay_sec && g_running; i++) sleep(1);
    if (!g_running) return NULL;

    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);
    uint64_t ts_us = (uint64_t)(tv_now.tv_sec - g_t_start.tv_sec) * 1000000ULL +
                     (uint64_t)tv_now.tv_usec;
    printf("[TEST] Injecting simulated FALL event\n");
    handle_fall_trigger(ts_us);

    /* 模拟摔倒事件也触发 UDP 转发到 HUD/App，用于测试 */
    char simulated_line[256];
    snprintf(simulated_line, sizeof(simulated_line),
             "IMU_ALERT type=fall reason=simulated seq=0 gps_valid=0 lat_1e7=0 lon_1e7=0");
    forward_imu_alert(simulated_line);

    return NULL;
}

/* ======================== 测试模式: 模拟 V2X 告警 ======================== */
static int g_test_v2x_delay_sec = 0;
static char g_test_v2x_direction[32] = "left_front";
static void *test_v2x_thread(void *arg) {
    (void)arg;
    if (g_test_v2x_delay_sec <= 0) return NULL;
    printf("[TEST] Simulating V2X alert (%s) after %d seconds...\n",
           g_test_v2x_direction, g_test_v2x_delay_sec);
    for (int i = 0; i < g_test_v2x_delay_sec && g_running; i++) sleep(1);
    if (!g_running) return NULL;

    printf("[TEST] Injecting simulated V2X event\n");
    handle_v2x_alert(g_test_v2x_direction);
    return NULL;
}

/* ======================== RPMsg 接收线程 (M33 alerts) ======================== */
static void *rpmsg_thread(void *arg) {
    (void)arg;

    int fd = open(RPMSG_DEVICE, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "[IMU] Cannot open %s: %s\n", RPMSG_DEVICE, strerror(errno));
        return NULL;
    }

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    cfsetospeed(&tty, RPMSG_BAUD);
    cfsetispeed(&tty, RPMSG_BAUD);
    tty.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
    tty.c_cflag |= CS8 | CREAD | CLOCAL;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY | IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
    tty.c_oflag &= ~OPOST & ~ONLCR;
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;
    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        fprintf(stderr, "[IMU] tcsetattr failed: %s\n", strerror(errno));
        close(fd);
        return NULL;
    }
    tcflush(fd, TCIOFLUSH);

    /* M33 需要收到 ready 消息后才开始发送告警 */
    sleep(1);
    if (write(fd, RPMSG_READY_MSG, strlen(RPMSG_READY_MSG)) < 0) {
        fprintf(stderr, "[IMU] Failed to send ready message: %s\n", strerror(errno));
    } else {
        tcdrain(fd);
        printf("[系统] [IMU] RPMsg ready message sent to M33\n");
    }

    char line[512];
    int idx = 0;
    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = { 0, 50000 };  /* 50ms timeout */
        int ret = select(fd + 1, &rfds, NULL, NULL, &tv);

        if (ret > 0 && FD_ISSET(fd, &rfds)) {
            char c;
            int n = read(fd, &c, 1);
            if (n > 0) {
                if (c == '\n' || c == '\r') {
                    if (idx > 0) {
                        line[idx] = '\0';

                        /* 解析 IMU 异常告警: IMU_ALERT ... type=... */
                        if (strstr(line, "IMU_ALERT") != NULL) {
                            struct timeval tv_now;
                            gettimeofday(&tv_now, NULL);
                            uint64_t ts_us = (uint64_t)(tv_now.tv_sec - g_t_start.tv_sec) * 1000000ULL +
                                             (uint64_t)tv_now.tv_usec;

                            /* 摔倒事件额外触发音频 + DVR 保存 */
                            if (strstr(line, "type=fall") != NULL) {
                                handle_fall_trigger(ts_us);
                            }

                            /* 所有 IMU 异常统一通过 UDP 转发到 HUD/App */
                            forward_imu_alert(line);
                        }
                        /* V2X 告警: 解析 direction 并播放定向语音 */
                        else if (strstr(line, "V2X_ALERT") != NULL) {
                            char direction[32] = "nearby";
                            const char *dir_start = strstr(line, "direction=");
                            if (dir_start != NULL) {
                                dir_start += strlen("direction=");
                                const char *dir_end = strchr(dir_start, ' ');
                                int len = (dir_end != NULL) ? (int)(dir_end - dir_start) : (int)strlen(dir_start);
                                if (len > 0 && len < (int)sizeof(direction)) {
                                    memcpy(direction, dir_start, len);
                                    direction[len] = '\0';
                                }
                            }
                            handle_v2x_alert(direction);
                        }

                        idx = 0;
                    }
                } else if (idx < (int)sizeof(line) - 1) {
                    line[idx++] = c;
                }
            }
        }
    }

    close(fd);
    printf("[系统] [IMU] RPMsg thread stopped\n");
    return NULL;
}

/* ======================== 主函数 ======================== */
int main(int argc, char *argv[]) {
    const char *camera_dev  = "/dev/video7";
    const char *uart_dev    = UART_DEVICE;
    const char *model_path  = "models/ssd_mobilenet_v2_fpnlite_10_256_int8_per_tensor.nb";
    const char *labels_path = "models/labels_coco_dataset_80.txt";
    float confidence = 0.60f;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) camera_dev = argv[++i];
        else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) uart_dev = argv[++i];
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) confidence = atof(argv[++i]);
        else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) model_path = argv[++i];
        else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) labels_path = argv[++i];
        else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) g_test_fall_delay_sec = atoi(argv[++i]);
        else if (strcmp(argv[i], "-V") == 0 && i + 1 < argc) g_test_v2x_delay_sec = atoi(argv[++i]);
        else if (strcmp(argv[i], "-x") == 0 && i + 1 < argc) {
            snprintf(g_test_v2x_direction, sizeof(g_test_v2x_direction), "%s", argv[++i]);
        }
        else if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) g_ttc_threshold = atof(argv[++i]);
        else if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) g_dist_threshold = atof(argv[++i]);
        else if (strcmp(argv[i], "--left-angle") == 0 && i + 1 < argc)
            g_angle_left_threshold = atof(argv[++i]);
        else if (strcmp(argv[i], "--right-angle") == 0 && i + 1 < argc)
            g_angle_right_threshold = atof(argv[++i]);
        else if (strcmp(argv[i], "--angle-sign") == 0 && i + 1 < argc)
            g_angle_direction_sign = atof(argv[++i]);
        else if (strcmp(argv[i], "--angle-alpha") == 0 && i + 1 < argc)
            g_angle_filter_alpha = atof(argv[++i]);
        else if (strcmp(argv[i], "--direction-samples") == 0 && i + 1 < argc)
            g_direction_stable_samples = atoi(argv[++i]);
        else if (strcmp(argv[i], "--radar-log-dir") == 0 && i + 1 < argc)
            snprintf(g_radar_log_dir, sizeof(g_radar_log_dir), "%s", argv[++i]);
        else if (strcmp(argv[i], "--ble-led-uart") == 0 && i + 1 < argc)
            snprintf(g_ble_led_uart, sizeof(g_ble_led_uart), "%s", argv[++i]);
        else if (strcmp(argv[i], "--no-ble-led") == 0)
            g_ble_led_enabled = false;
        else if (strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [-d camera] [-u uart] [-c conf] [-T ttc] [-D dist]\n"
                   "          [--left-angle deg] [--right-angle deg]\n"
                   "          [--angle-sign -1|1]\n"
                   "          [--angle-alpha 0..1] [--direction-samples n]\n"
                   "          [--radar-log-dir path]\n"
                   "          [--ble-led-uart path] [--no-ble-led]\n"
                   "          [-t fall_delay] [-V v2x_delay] [-x v2x_dir] [-h]\n",
                   argv[0]);
            printf("  v2x_dir: nearby|left_front|right_front|left|right\n");
            printf("  Direction uses rider_angle = sensor_angle * angle_sign\n");
            printf("  rider_angle <= left is LEFT; rider_angle >= right is RIGHT\n");
            printf("  Defaults: TTC=%.1fs distance=%.1fm left=%.1fdeg right=%.1fdeg\n",
                   TTC_THRESHOLD_DEFAULT, (float)DIST_THRESHOLD_DEFAULT,
                   ANGLE_LEFT_DEFAULT, ANGLE_RIGHT_DEFAULT);
            return 0;
        }
    }

    if (g_ttc_threshold <= 0.0f || g_dist_threshold <= 0.0f) {
        fprintf(stderr, "[RADAR] TTC and distance thresholds must be positive\n");
        return 2;
    }
    if (g_angle_left_threshold >= g_angle_right_threshold) {
        fprintf(stderr,
                "[RADAR] left-angle must be smaller than right-angle\n");
        return 2;
    }
    if (fabsf(fabsf(g_angle_direction_sign) - 1.0f) > 0.001f) {
        fprintf(stderr, "[RADAR] angle-sign must be -1 or 1\n");
        return 2;
    }
    if (g_angle_filter_alpha <= 0.0f || g_angle_filter_alpha > 1.0f) {
        fprintf(stderr, "[RADAR] angle-alpha must be in (0, 1]\n");
        return 2;
    }
    if (g_direction_stable_samples < 1 ||
        g_direction_stable_samples > 20) {
        fprintf(stderr, "[RADAR] direction-samples must be between 1 and 20\n");
        return 2;
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* 当日志被重定向到文件时，默认全缓冲会导致显示严重滞后；改为行缓冲 */
    setvbuf(stdout, NULL, _IOLBF, BUFSIZ);
    setvbuf(stderr, NULL, _IOLBF, BUFSIZ);
    ble_risk_configure(g_ble_led_uart, g_ble_led_enabled);

    printf("========================================\n");
    printf(" Radar + Camera NPU Fusion + DVR (v4)\n");
    printf("========================================\n");
    printf("Radar:    %s @ %d bps\n", uart_dev, BAUDRATE);
    printf("Camera:   %s\n", camera_dev);
    printf("Model:    %s\n", model_path);
    printf("Conf:     %.2f\n", confidence);
    printf("TTC thr:  %.1f s\n", g_ttc_threshold);
    printf("Dist thr: %.1f m\n", g_dist_threshold);
    printf("Angles:   LEFT <= %.1f deg, RIGHT >= %.1f deg\n",
           g_angle_left_threshold, g_angle_right_threshold);
    printf("Angle map:rider = sensor * %.0f\n", g_angle_direction_sign);
    printf("Dir filt: alpha=%.2f, stable samples=%d\n",
           g_angle_filter_alpha, g_direction_stable_samples);
    printf("Radar log:%s\n", g_radar_log_dir);
    printf("BLE LEDs: %s%s\n",
           g_ble_led_enabled ? g_ble_led_uart : "disabled",
           g_ble_led_enabled ? " @ 115200" : "");
    printf("LED:      PD11 via %s\n", GPIO_CHIP_DEV);
    printf("DVR:      %s (pre=%ds post=%ds)\n", DVR_BASE_DIR, DVR_SAVE_BEFORE_SEC, DVR_SAVE_AFTER_SEC);
    printf("========================================\n\n");

    /* 0. 检查 DVR 依赖: 视频编码器 (gst-launch-1.0 / ffmpeg / avconv) + TF 卡 */
    printf("[系统] [DVR] Checking dependencies...\n");
    dvr_has_encoder = (system("which gst-launch-1.0 >/dev/null 2>&1") == 0) ||
                      (system("which ffmpeg >/dev/null 2>&1") == 0) ||
                      (system("which avconv >/dev/null 2>&1") == 0);
    if (!dvr_has_encoder) {
        printf("[系统] [DVR] WARNING: No video encoder found! Video encoding will be disabled.\n");
        printf("[系统] [DVR] Install: apt-get install gstreamer1.0-tools ffmpeg\n");
    } else {
        printf("[系统] [DVR] encoder: OK (gst-launch-1.0 / ffmpeg / avconv)\n");
    }

    struct stat st;
    dvr_tf_ok = (stat(DVR_BASE_DIR, &st) == 0 && S_ISDIR(st.st_mode));
    if (!dvr_tf_ok) {
        /* 尝试创建 */
        if (mkdir(DVR_BASE_DIR, 0777) == 0) {
            dvr_tf_ok = 1;
            printf("[系统] [DVR] Created: %s\n", DVR_BASE_DIR);
        } else {
            printf("[系统] [DVR] WARNING: TF card not available at %s (%s)\n", DVR_BASE_DIR, strerror(errno));
            printf("[系统] [DVR] DVR recording will be disabled until TF card is inserted.\n");
        }
    } else {
        printf("[系统] [DVR] TF card: OK (%s)\n", DVR_BASE_DIR);
    }

    /* 1. GPIO LED: PD11, 用于雷达+NPU/IMU 告警闪烁 */
    if (gpio_init() != 0)
        fprintf(stderr, "[系统] [LED] init failed, LED disabled\n");

    pthread_t led_tid;
    pthread_create(&led_tid, NULL, led_thread, NULL);

    /* 2. 雷达: 打开串口、设置波特率、初始化 BSD 工作模式 */
    kill_device_holders(uart_dev);
    int radar_fd = open(uart_dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (radar_fd < 0) {
        fprintf(stderr, "[系统] Cannot open radar %s: %s\n", uart_dev, strerror(errno));
        g_running = 0; pthread_join(led_tid, NULL); gpio_deinit();
        return 1;
    }
    if (set_uart(radar_fd, BAUDRATE) != 0) {
        close(radar_fd); g_running = 0; pthread_join(led_tid, NULL); gpio_deinit();
        return 1;
    }
    radar_init(radar_fd);

    /* 3. 摄像头 + NPU: 采集图像并做目标检测, 用于验证雷达目标是否为真实道路使用者 */
    kill_device_holders(camera_dev);
    camera_t cam;
    NpuDetector *detector = NULL;
    int camera_ok = 0;
    uint8_t *rgb_full = NULL, *rgb_nn = NULL;
    int nn_w = 0, nn_h = 0;

    if (camera_open(&cam, camera_dev, 1280, 720) == 0) {
        dvr_camera_pixelformat = cam.pixelformat;
        if (camera_start(&cam) == 0) {
            detector = new NpuDetector(model_path, labels_path, confidence, 0.45f);
            nn_w = detector->get_input_width();
            nn_h = detector->get_input_height();
            rgb_full = (uint8_t *)malloc(cam.width * cam.height * 3);
            rgb_nn = (uint8_t *)malloc(nn_w * nn_h * 3);
            if (rgb_full && rgb_nn) {
                camera_ok = 1;
                printf("[FUSION] Camera + NPU enabled\n");
            } else {
                printf("[FUSION] NPU buffer alloc failed\n");
                camera_stop(&cam); camera_close(&cam);
                delete detector; detector = NULL;
            }
        } else {
            printf("[FUSION] Camera start failed\n");
            camera_close(&cam);
        }
    } else {
        printf("[FUSION] Camera not available, radar-only mode\n");
    }
    if (!camera_ok) printf("[FUSION] Running in RADAR-ONLY mode (no DVR)\n");
    g_camera_ok_global = camera_ok;

    printf("\n[系统] [FUSION] Initialization complete, entering main loop\n");
    if (camera_ok) {
        printf("[系统] [FUSION] Mode: camera + NPU + radar + DVR\n");
    } else {
        printf("[系统] [FUSION] Mode: radar-only (no camera/DVR)\n");
    }
    printf("[系统] [FUSION] Running... Press Ctrl+C to stop.\n\n");

    /* 必须在线程启动前打开共享传感器日志，保证首条 M33 事件不丢失。 */
    sensor_telemetry_init();

    /* 4. 启动 RPMsg 接收线程 (M33 IMU/V2X alerts) */
    pthread_t rpmsg_tid;
    pthread_create(&rpmsg_tid, NULL, rpmsg_thread, NULL);

    /* 5. 注册异常路况骨传导播报处理函数
     *    无网络时会按关键词播放本地固定提示音 */
    nav_tts_set_danger_text_handler(nav_tts_speak_danger);

    /* 6. 启动 HUD 导航语音接收线程 (UDP 8888) */
    if (nav_tts_start() != 0) {
        fprintf(stderr, "[系统] [NAV] Failed to start navigation receiver\n");
    }

    /* 7. 启动测试线程 (如果指定了 -t 或 -V) */
    pthread_t test_fall_tid;
    if (g_test_fall_delay_sec > 0) {
        pthread_create(&test_fall_tid, NULL, test_fall_thread, NULL);
    }
    pthread_t test_v2x_tid;
    if (g_test_v2x_delay_sec > 0) {
        pthread_create(&test_v2x_tid, NULL, test_v2x_thread, NULL);
    }

    /* 5. 主循环: 单线程轮询雷达 + 定时采集摄像头 + NPU 推理 + DVR 缓冲 */
    /*
     * 主循环数据流:
     *   1) 按 25fps 定时从摄像头取 MJPEG 帧
     *   2) DVR 把帧写入 TF 卡循环缓冲 (见 dvr_start/dvr_save_frame)
     *   3) 每 10 帧选 1 帧做 NPU 推理, 判断是否有道路使用者
     *   4) NPU 看到目标 → 开始/继续 DVR 缓冲
     *   5) NPU 连续 N 帧没看到目标 → 停止缓冲 (未触发保存时)
     *   6) 雷达检测到 BSD 目标 → 结合 NPU 状态判断是否为真实危险
     *   7) 雷达告警 + NPU 确认 → LED 闪烁 + 触发 DVR 保存 + 播放碰撞告警音
     */
    uint8_t rx_buf[512];
    int rx_len = 0;
    int poll_cnt = 0;
    int npu_frame_count = 0;

    int target_active   = 0;   /* 雷达当前是否有有效目标 */
    int npu_has_target  = 0;   /* NPU 当前是否看到道路用户 (控制 DVR 缓冲) */
    int npu_confirm_cnt = 0;   /* NPU 连续确认帧计数 */
    int npu_deny_cnt    = 0;   /* NPU 连续否认帧计数 */
    int npu_confirmed   = 0;   /* NPU 已确认真实道路使用者 */
    int npu_denied      = 0;   /* NPU 已判断为雷达虚警 */

    struct timeval t_last_bsd, t_last_capture;
    gettimeofday(&g_t_start, NULL);
    t_last_bsd = g_t_start;
    t_last_capture = g_t_start;
    radar_telemetry_init();
    radar_telemetry_publish_empty();

    while (g_running) {
        /* 回收异步编码子进程 */
        if (dvr_encoder_pid > 0) {
            int enc_status;
            pid_t reaped = waitpid(dvr_encoder_pid, &enc_status, WNOHANG);
            if (reaped == dvr_encoder_pid) {
                if (WIFEXITED(enc_status)) {
                    printf("[保存] [DVR] Encoder finished (exit=%d)\n", WEXITSTATUS(enc_status));
                }
                dvr_encoder_pid = 0;
                dvr_encoding = 0;
                dvr_save_triggered = 0;
                dvr_trigger_time_us = 0;
                /* 重置告警状态，准备下一轮触发 */
                g_radar_npu_alert = 0;
                g_imu_fall_alert = 0;
                g_v2x_alert = 0;
                g_imu_fall_time_us = 0;
                npu_confirm_cnt = 0;
                npu_deny_cnt = 0;
                npu_confirmed = 0;
                npu_denied = 0;
                printf("[保存] [DVR] State reset, ready for next trigger\n");
            } else if (reaped < 0) {
                dvr_encoder_pid = 0;
                dvr_encoding = 0;
            }
        }

        /* 计算下一次捕获的时间 */
        struct timeval tv_now;
        gettimeofday(&tv_now, NULL);
        long elapsed_us = (tv_now.tv_sec - t_last_capture.tv_sec) * 1000000L +
                          (tv_now.tv_usec - t_last_capture.tv_usec);
        long wait_us = DVR_CAPTURE_INTERVAL_US - elapsed_us;
        if (wait_us < 0) wait_us = 0;
        if (wait_us > 500000) wait_us = 500000; /* 最多等 500ms */

        struct timeval tv = { wait_us / 1000000, wait_us % 1000000 };
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(radar_fd, &rfds);
        int ret = select(radar_fd + 1, &rfds, NULL, NULL, &tv);

        gettimeofday(&tv_now, NULL);
        long since_capture_us = (tv_now.tv_sec - t_last_capture.tv_sec) * 1000000L +
                                (tv_now.tv_usec - t_last_capture.tv_usec);

        /* 固定频率捕获帧 (用于 DVR 和 NPU) */
        if (camera_ok && detector && since_capture_us >= DVR_CAPTURE_INTERVAL_US) {
            uint8_t *jpeg_buf;
            unsigned int jpeg_len;
            if (camera_capture(&cam, &jpeg_buf, &jpeg_len) == 0) {
                t_last_capture = tv_now;
                uint64_t ts_us = (uint64_t)(tv_now.tv_sec - g_t_start.tv_sec) * 1000000ULL +
                                 (uint64_t)tv_now.tv_usec;

                /* DVR: 保存帧 */
                if (dvr_recording && !dvr_encoding) {
                    dvr_save_frame(jpeg_buf, jpeg_len, ts_us);
                }

                /* DVR: 触发后继续录制 post 秒数 */
                if (dvr_save_triggered && dvr_recording && !dvr_encoding) {
                    /* 触发可能发生在当前帧时间戳计算之后，避免无符号下溢 */
                    uint64_t elapsed = (ts_us >= dvr_trigger_time_us)
                                           ? (ts_us - dvr_trigger_time_us)
                                           : 0;
                    if (elapsed > (uint64_t)DVR_SAVE_AFTER_SEC * 1000000ULL) {
                        printf("[保存] [DVR] Post-trigger recording complete (%llu ms)\n",
                               (unsigned long long)(elapsed / 1000));
                        /* 停止录制并编码 */
                        dvr_stop();
                        dvr_encode_mp4();
                    }
                }

                /* NPU: 每 10 帧推理一次, 优先保证录像流畅 */
                npu_frame_count++;
                if (npu_frame_count % 10 == 0) {
                    struct timeval tv_npu_start, tv_npu_end;
                    gettimeofday(&tv_npu_start, NULL);
                    int dec_w, dec_h;
                    if (jpeg_decode_rgb_silent(jpeg_buf, jpeg_len, rgb_full, &dec_w, &dec_h) == 0) {
                        resize_rgb(rgb_full, dec_w, dec_h, rgb_nn, nn_w, nn_h);
                        frame_results_t results = detector->detect(rgb_nn);

                        int has_road = 0;
                        int road_count = 0;
                        const char *best_label = "";
                        float best_score = 0;
                        for (size_t i = 0; i < results.objects.size(); i++) {
                            if (is_road_user(results.objects[i].class_index)) {
                                has_road = 1;
                                road_count++;
                                if (results.objects[i].score > best_score) {
                                    best_score = results.objects[i].score;
                                    best_label = detector->get_label(results.objects[i].class_index).c_str();
                                }
                            }
                        }

                        if (has_road && best_label[0]) {
                            snprintf(g_last_road_user_label, sizeof(g_last_road_user_label), "%s", best_label);
                            g_last_road_user_score = best_score;
                        }

                        double t = (tv_now.tv_sec - g_t_start.tv_sec) +
                                   (tv_now.tv_usec - g_t_start.tv_usec) / 1000000.0;

                        /* === NPU 目标状态跟踪 (控制缓冲) === */
                        if (has_road) {
                            if (!npu_has_target) {
                                npu_has_target = 1;
                                /* 新目标出现: 重置所有状态 */
                                npu_denied = 0;
                                npu_confirmed = 0;
                                npu_confirm_cnt = 0;
                                npu_deny_cnt = 0;
                                printf("[目标] [%6.1fs] NPU: ROAD USER DETECTED (%s %.2f)\n", t, best_label, best_score);
                            }
                            npu_confirm_cnt++;
                            npu_deny_cnt = 0;
                            /*
                             * 不只在 0→1 边沿启动一次：若首次因 TF/目录瞬态失败，
                             * 目标持续存在时每 2 秒重试，保证碰撞前尽量已有预缓存。
                             */
                            if (camera_ok && !dvr_recording && !dvr_encoding) {
                                dvr_ensure_started(ts_us, "npu_target", 0);
                            }
                            if (npu_confirm_cnt >= NPU_CONFIRM_FRAMES && !npu_confirmed && !npu_denied) {
                                npu_confirmed = 1;
                                printf("[目标] [%6.1fs] NPU CONFIRMED: Real road user!\n", t);
                            }
                        } else {
                            npu_deny_cnt++;
                            npu_confirm_cnt = 0;
                            if (npu_deny_cnt >= NPU_DENY_FRAMES) {
                                if (npu_has_target) {
                                    npu_has_target = 0;
                                    printf("[目标] [%6.1fs] NPU LOST TARGET\n", t);
                                    /* 停止缓冲 (如果没有触发保存) */
                                    if (!dvr_save_triggered && dvr_recording && !dvr_encoding) {
                                        dvr_stop();
                                    }
                                }
                                if (!npu_denied) {
                                    npu_denied = 1;
                                    npu_confirmed = 0;
                                    printf("[目标] [%6.1fs] NPU DENIED: Radar false trigger\n", t);
                                }
                            }
                        }

                        char npu_details[192];
                        snprintf(npu_details, sizeof(npu_details),
                                 "objects=%zu inference_ms=%.1f confirmed=%d active=%d",
                                 results.objects.size(), results.inference_time_ms,
                                 npu_confirmed, npu_has_target);
                        sensor_event_log("camera_npu", "npu_inference",
                                         has_road ? "target" : "clear", NULL,
                                         has_road ? best_label : NULL,
                                         has_road ? best_score : -1.0f,
                                         road_count, -1, NULL, npu_details);

                        /* === 显示 NPU 中间结果 (调试级别，不进终端) === */
                        if (npu_has_target || target_active) {
                            printf("[调试] [%6.1fs] NPU(%dms): %zu objects, road_user=%s",
                                   t, (int)results.inference_time_ms, results.objects.size(),
                                   has_road ? "YES" : "no");
                            if (has_road) {
                                printf(" (%s %.2f)", best_label, best_score);
                            }
                            printf("\n");
                        }

                        /* 注意: DVR 保存触发和 LED 控制在雷达段处理,
                           确保只有 radar.should_alert + NPU 确认才触发 */
                    }
                    gettimeofday(&tv_npu_end, NULL);
                    long npu_us = (tv_npu_end.tv_sec - tv_npu_start.tv_sec) * 1000000L +
                                  (tv_npu_end.tv_usec - tv_npu_start.tv_usec);
                    printf("[调试] [NPU] inference cycle took %ld ms\n", npu_us / 1000);
                }
            }
        }

        /* 雷达轮询 */
        poll_cnt++;
        if (poll_cnt >= 60) { poll_cnt = 0; send_cmd(radar_fd, 1, 0x10, NULL, 0); }

        /* 目标消失检测 */
        if (target_active) {
            gettimeofday(&tv_now, NULL);
            double elapsed = (tv_now.tv_sec - t_last_bsd.tv_sec) +
                             (tv_now.tv_usec - t_last_bsd.tv_usec) / 1000000.0;
            if (elapsed > TARGET_TIMEOUT_S) {
                double t = (tv_now.tv_sec - g_t_start.tv_sec) +
                           (tv_now.tv_usec - g_t_start.tv_usec) / 1000000.0;
                printf("[目标] [%6.1fs] TARGET GONE (%.1fs timeout)\n", t, elapsed);
                target_active   = 0;
                g_radar_npu_alert = 0;
                g_led_alert     = 0;
                radar_telemetry_publish_empty();
                ble_risk_update(BLE_RISK_CLEAR);

                /* 注意: 不在这里停止 DVR!
                   post-trigger 15s 由帧捕获部分的定时器控制,
                   NPU 丢失目标后由 NPU 逻辑处理缓冲停止 */
            }
        }

        /* 无目标时也维持 1 Hz 状态心跳，避免 Dashboard 把正常空闲误判为离线。 */
        if (!target_active) {
            struct timeval tv_heartbeat;
            gettimeofday(&tv_heartbeat, NULL);
            uint64_t heartbeat_ms =
                (uint64_t)tv_heartbeat.tv_sec * 1000ULL +
                (uint64_t)tv_heartbeat.tv_usec / 1000ULL;
            if (heartbeat_ms - g_radar_last_publish_ms >= 1000ULL)
                radar_telemetry_publish_empty();
            ble_risk_update(BLE_RISK_CLEAR);
        }

        /* 处理雷达数据 */
        if (ret > 0 && FD_ISSET(radar_fd, &rfds)) {
            int n = read(radar_fd, rx_buf + rx_len, sizeof(rx_buf) - rx_len);
            if (n > 0) rx_len += n;

            while (rx_len >= 4) {
                uint8_t head = rx_buf[0];
                int frame_total = -1;
                if (head == HEAD_REPORT) {
                    if (rx_len < 3) break;
                    frame_total = 2 + rx_buf[1] + 1;
                } else if (head == HEAD_REPLY) {
                    if (rx_len < 3) break;
                    frame_total = 5 + rx_buf[2];
                } else {
                    int i = 1;
                    for (; i < rx_len; i++)
                        if (rx_buf[i] == HEAD_REPORT || rx_buf[i] == HEAD_REPLY) break;
                    memmove(rx_buf, rx_buf + i, rx_len - i);
                    rx_len -= i;
                    continue;
                }
                if (frame_total > (int)sizeof(rx_buf) || frame_total > rx_len) break;

                uint8_t frame[512];
                memcpy(frame, rx_buf, frame_total);
                radar_result_t radar;
                if (process_radar_frame(frame, frame_total, &radar) == 1) {
                    gettimeofday(&tv_now, NULL);
                    double t = (tv_now.tv_sec - g_t_start.tv_sec) +
                               (tv_now.tv_usec - g_t_start.tv_usec) / 1000000.0;

                    if (radar.has_target) {
                        t_last_bsd = tv_now;
                    }
                    if (radar.has_target && !target_active) {
                        printf("[目标] [%6.1fs] TARGET ON\n", t);
                        target_active = 1;
                        /* NPU 状态由 NPU 推理逻辑独立管理，不在此重置 */
                    }

                    const char *target_label = (npu_has_target && g_last_road_user_label[0])
                                                   ? g_last_road_user_label : "unknown";
                    const radar_target_t *dangerous =
                        radar.dangerous_index >= 0
                            ? &radar.targets[radar.dangerous_index]
                            : NULL;
                    float ttc_val =
                        dangerous != NULL ? dangerous->ttc : -1.0f;

                    /* 只在危险目标/方向/距离/TTC/告警变化时打印，避免刷屏 */
                    static char s_last_label[32] = "";
                    static int  s_last_obj_id = -2;
                    static float s_last_dist = -1.0f;
                    static float s_last_ttc = -2.0f;
                    static radar_direction_t s_last_direction = RADAR_DIR_UNKNOWN;
                    static int  s_last_alert = -1;
                    int changed = (strcmp(target_label, s_last_label) != 0) ||
                                  (radar.dangerous_obj_id != s_last_obj_id) ||
                                  (dangerous != NULL &&
                                   fabsf(dangerous->distance - s_last_dist) > 0.05f) ||
                                  (fabsf(ttc_val - s_last_ttc) > 0.05f) ||
                                  (dangerous != NULL &&
                                   dangerous->direction != s_last_direction) ||
                                  ((int)radar.should_alert != s_last_alert);
                    if (changed) {
                        snprintf(s_last_label, sizeof(s_last_label), "%s", target_label);
                        s_last_obj_id = radar.dangerous_obj_id;
                        s_last_dist = dangerous != NULL
                                          ? dangerous->distance
                                          : -1.0f;
                        s_last_ttc = ttc_val;
                        s_last_direction = dangerous != NULL
                                               ? dangerous->direction
                                               : RADAR_DIR_UNKNOWN;
                        s_last_alert = radar.should_alert;
                        if (dangerous != NULL) {
                            printf("[目标] [%6.1fs] target=%s obj=%d dist=%.1fm "
                                   "speed=%.1fm/s angle=%.1fdeg dir=%s "
                                   "ttc=%.1fs alert=%s\n",
                                   t, target_label, dangerous->obj_id,
                                   dangerous->distance, dangerous->velocity,
                                   dangerous->angle,
                                   radar_direction_name(dangerous->direction),
                                   ttc_val,
                                   radar.should_alert ? "YES" : "no");
                        }
                    }

                    /* DVR + 告警状态: 雷达告警 + NPU 确认 → 触发 */
                    if (camera_ok) {
                        if (radar.should_alert && npu_confirmed && !npu_denied) {
                            /* 雷达告警 + NPU 确认 → 真正碰撞风险 */
                            uint64_t collision_trigger_us =
                                (uint64_t)(tv_now.tv_sec - g_t_start.tv_sec) *
                                    1000000ULL +
                                (uint64_t)tv_now.tv_usec;
                            g_radar_npu_alert = 1;
                            /*
                             * 正常情况下 NPU 目标出现后已经启动循环缓存。如果 TF 卡
                             * 刚挂载或首次启动失败，则在真正碰撞告警到达时再强制尝试，
                             * 避免“雷达/NPU 已告警但因为无缓存而静默不保存”。
                             */
                            if (!dvr_recording && !dvr_encoding) {
                                if (dvr_ensure_started(
                                        collision_trigger_us,
                                        "radar_npu_collision", 1) == 0) {
                                    printf("[DVR] 碰撞告警时补启动录像，"
                                           "本次视频可能缺少告警前缓存\n");
                                }
                            }
                            if (dvr_recording && !dvr_save_triggered && !dvr_encoding) {
                                printf("[告警] [%6.1fs] ALERT: COLLISION RISK - NPU CONFIRMED\n", t);
                                dvr_trigger_save(collision_trigger_us,
                                                 "radar_npu_collision");
                                play_alert_sound("collision");
                            }
                        } else {
                            g_radar_npu_alert = 0;
                        }
                    } else {
                        /* 纯雷达模式 (无摄像头) */
                        if (radar.should_alert) {
                            printf("[告警] [%6.1fs] ALERT: COLLISION RISK (radar-only)\n", t);
                            g_radar_npu_alert = 1;
                        } else {
                            g_radar_npu_alert = 0;
                        }
                    }

                    /*
                     * 只转发已经通过现有雷达/NPU逻辑确认的最终碰撞风险。
                     * CENTER（或方向暂未稳定）点亮两侧，CLEAR 熄灭两侧。
                     */
                    BleRiskState ble_state = BLE_RISK_CLEAR;
                    if (g_radar_npu_alert && dangerous != NULL) {
                        if (dangerous->direction == RADAR_DIR_LEFT)
                            ble_state = BLE_RISK_LEFT;
                        else if (dangerous->direction == RADAR_DIR_RIGHT)
                            ble_state = BLE_RISK_RIGHT;
                        else
                            ble_state = BLE_RISK_CENTER;
                    }
                    ble_risk_update(ble_state);
                    radar_telemetry_publish(&radar, g_radar_npu_alert);
                }
                memmove(rx_buf, rx_buf + frame_total, rx_len - frame_total);
                rx_len -= frame_total;
            }
            if (rx_len >= (int)sizeof(rx_buf)) rx_len = 0;
        }

        /* 统一 LED 控制: 雷达+NPU 碰撞告警 / IMU 摔倒告警
         * V2X 只通过语音提示，不再驱动 LED，避免误闪后无法熄灭 */
        g_led_alert = g_radar_npu_alert || g_imu_fall_alert;
    }

    printf("\n[系统] [FUSION] Stopping...\n");
    ble_risk_shutdown();

    /* DVR 清理 */
    if (dvr_recording && !dvr_encoding) {
        dvr_stop();
        if (dvr_save_triggered && dvr_frame_count > 0) {
            dvr_encode_mp4();
        }
    }
    /* 如果 post-trigger 录制中就被打断, 也编码 */
    if (dvr_save_triggered && dvr_frame_count > 0 && !dvr_encoding && dvr_encoder_pid == 0) {
        dvr_encode_mp4();
    }

    /* 等待异步编码完成 */
    if (dvr_encoder_pid > 0) {
        printf("[保存] [DVR] Waiting for encoder (pid=%d)...\n", dvr_encoder_pid);
        int enc_status;
        waitpid(dvr_encoder_pid, &enc_status, 0);
        dvr_encoder_pid = 0;
        dvr_encoding = 0;
    }

    g_led_alert = 0;
    pthread_join(led_tid, NULL);
    pthread_join(rpmsg_tid, NULL);
    nav_tts_stop();
    if (g_test_fall_delay_sec > 0) pthread_join(test_fall_tid, NULL);
    if (g_test_v2x_delay_sec > 0) pthread_join(test_v2x_tid, NULL);
    gpio_deinit();

    if (camera_ok) {
        camera_stop(&cam);
        camera_close(&cam);
        free(rgb_full);
        free(rgb_nn);
        delete detector;
    }
    close(radar_fd);
    radar_telemetry_close();
    sensor_telemetry_close();

    printf("[系统] [FUSION] Done.\n");
    return 0;
}
