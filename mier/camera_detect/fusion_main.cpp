/**
 * fusion_main.cpp - 雷达 + 摄像头 NPU 融合验证程序
 *
 * 架构:
 *   雷达 (radar_link) → 命名管道 /tmp/dvr_trigger_pipe → 融合程序
 *                                                          ├─ 管道读取线程
 *                                                          └─ 摄像头 + NPU 检测线程
 *
 * 融合逻辑:
 *   TARGET_ON  → 目标出现, 持续 NPU 检测, 判断是否为道路用户
 *   COLLISION  → 碰撞风险, 若 NPU 已确认道路用户则告警有效
 *   TARGET_OFF → 目标消失, 输出本次统计
 *
 * 道路用户判定:
 *   连续 NPU_CONFIRM_NEEDED 帧检测到 person/bicycle/car/motorbike/bus/truck → CONFIRMED
 *   连续 NPU_DENY_NEEDED 帧未检测到 → DENIED (雷达误触发)
 *
 * Usage: ./fusion_detect [-d /dev/video7] [-c 0.60]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <errno.h>

#include "camera.h"
#include "npu_detect.h"

/* JPEG decoder */
extern "C" int jpeg_decode_rgb(const unsigned char *jpeg_data, unsigned long jpeg_size,
                                unsigned char *out_rgb, int *out_width, int *out_height);

/* ============ 配置 ============ */
#define PIPE_PATH           "/tmp/dvr_trigger_pipe"
#define NPU_CONFIRM_NEEDED  2    /* 连续检测到道路用户N帧 → 确认 */
#define NPU_DENY_NEEDED     3    /* 连续未检测到N帧 → 否认 */

/* 道路用户标签 (COCO 80-class) */
static const int ROAD_USER_CLASSES[] = {
    1,   /* person */
    2,   /* bicycle */
    3,   /* car */
    4,   /* motorcycle */
    5,   /* airplane (ignore) */
    6,   /* bus */
    7,   /* train */
    8,   /* truck */
};
static const int ROAD_USER_COUNT = sizeof(ROAD_USER_CLASSES) / sizeof(ROAD_USER_CLASSES[0]);

/* ============ 融合状态 ============ */
typedef enum {
    FUSION_IDLE      = 0,
    FUSION_TARGET_ON = 1,   /* 雷达发现目标, 正在NPU验证 */
    FUSION_CONFIRMED = 2,   /* NPU确认为道路用户 */
    FUSION_DENIED    = 3,   /* NPU否认, 雷达误触发 */
} fusion_state_t;

typedef struct {
    fusion_state_t state;
    int confirm_count;
    int deny_count;
    int total_detections;
    int road_user_detections;
    int collision_received;
    int64_t target_on_time_us;
    char last_detected_label[128];
    float last_detected_score;
} fusion_ctx_t;

/* ============ 全局变量 ============ */
static volatile int g_running = 1;
static volatile int g_radar_target_on = 0;
static volatile int g_radar_collision = 0;
static volatile int g_radar_target_off = 0;
static pthread_mutex_t g_radar_mutex = PTHREAD_MUTEX_INITIALIZER;

static void sig_handler(int sig) { (void)sig; g_running = 0; }

/* ============ 管道读取线程 ============ */
static void *pipe_reader_thread(void *arg) {
    (void)arg;
    char buf[256];

    while (g_running) {
        int fd = open(PIPE_PATH, O_RDONLY);
        if (fd < 0) {
            /* 管道不存在, 等待 radar_link 启动 */
            usleep(500000);
            continue;
        }

        while (g_running) {
            fd_set rfds;
            struct timeval tv = {1, 0};
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            int ret = select(fd + 1, &rfds, NULL, NULL, &tv);
            if (ret < 0) break;
            if (ret == 0) continue;

            memset(buf, 0, sizeof(buf));
            int n = read(fd, buf, sizeof(buf) - 1);
            if (n <= 0) break;

            /* 去掉末尾换行 */
            while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';

            pthread_mutex_lock(&g_radar_mutex);
            if (strcmp(buf, "TARGET_ON") == 0) {
                g_radar_target_on = 1;
                g_radar_target_off = 0;
                g_radar_collision = 0;
            } else if (strcmp(buf, "COLLISION") == 0) {
                g_radar_collision = 1;
            } else if (strcmp(buf, "TARGET_OFF") == 0) {
                g_radar_target_off = 1;
                g_radar_target_on = 0;
                g_radar_collision = 0;
            }
            pthread_mutex_unlock(&g_radar_mutex);
        }
        close(fd);
    }
    return NULL;
}

/* ============ 道路用户判断 ============ */
static int is_road_user(int class_index) {
    for (int i = 0; i < ROAD_USER_COUNT; i++) {
        if (class_index == ROAD_USER_CLASSES[i]) return 1;
    }
    return 0;
}

/* ============ 简单缩放 ============ */
static void resize_rgb(const uint8_t *src, int sw, int sh,
                       uint8_t *dst, int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        int sy = y * sh / dh;
        for (int x = 0; x < dw; x++) {
            int sx = x * sw / dw;
            int si = (sy * sw + sx) * 3;
            int di = (y * dw + x) * 3;
            dst[di + 0] = src[si + 0];
            dst[di + 1] = src[si + 1];
            dst[di + 2] = src[si + 2];
        }
    }
}

/* ============ 主函数 ============ */
int main(int argc, char *argv[]) {
    const char *device = "/dev/video7";
    const char *model_path = "models/ssd_mobilenet_v2_fpnlite_10_256_int8_per_tensor.nb";
    const char *labels_path = "models/labels_coco_dataset_80.txt";
    float confidence = 0.60f;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) device = argv[++i];
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) confidence = atof(argv[++i]);
        else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) model_path = argv[++i];
        else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) labels_path = argv[++i];
        else if (strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [-d /dev/video7] [-c 0.60] [-m model.nb] [-l labels.txt]\n", argv[0]);
            return 0;
        }
    }

    printf("========================================\n");
    printf(" Radar + Camera NPU Fusion\n");
    printf("========================================\n");
    printf("Device:   %s\n", device);
    printf("Model:    %s\n", model_path);
    printf("Conf:     %.2f\n", confidence);
    printf("Pipe:     %s\n", PIPE_PATH);
    printf("========================================\n\n");

    /* 创建命名管道 */
    unlink(PIPE_PATH);
    if (mkfifo(PIPE_PATH, 0666) < 0) {
        perror("mkfifo");
        return 1;
    }
    printf("[FUSION] Named pipe created: %s\n", PIPE_PATH);
    printf("[FUSION] Waiting for radar_link to connect...\n\n");

    /* 先启动管道读取线程 (必须在NPU加载之前, 否则错过雷达初始化事件) */
    pthread_t pipe_thread;
    pthread_create(&pipe_thread, NULL, pipe_reader_thread, NULL);

    /* 初始化 NPU */
    NpuDetector detector(model_path, labels_path, confidence, 0.45f);
    int nn_w = detector.get_input_width();
    int nn_h = detector.get_input_height();

    /* 打开摄像头 */
    camera_t cam;
    if (camera_open(&cam, device, 1280, 720) < 0) {
        unlink(PIPE_PATH);
        return 1;
    }
    if (camera_start(&cam) < 0) {
        camera_close(&cam);
        unlink(PIPE_PATH);
        return 1;
    }

    /* 分配缓冲区 */
    int rgb_size = cam.width * cam.height * 3;
    uint8_t *rgb_full = (uint8_t *)malloc(rgb_size);
    uint8_t *rgb_nn = (uint8_t *)malloc(nn_w * nn_h * 3);
    if (!rgb_full || !rgb_nn) {
        fprintf(stderr, "[FUSION] malloc failed\n");
        camera_stop(&cam); camera_close(&cam);
        unlink(PIPE_PATH);
        return 1;
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("[FUSION] Running... Press Ctrl+C to stop.\n");
    printf("[FUSION] Waiting for radar TARGET_ON...\n\n");

    fusion_ctx_t fusion;
    memset(&fusion, 0, sizeof(fusion));
    fusion.state = FUSION_IDLE;

    int frame_count = 0;
    struct timeval t_start;
    gettimeofday(&t_start, NULL);

    while (g_running) {
        /* 读取雷达事件 */
        int radar_on = 0, radar_collision = 0, radar_off = 0;
        pthread_mutex_lock(&g_radar_mutex);
        radar_on = g_radar_target_on;
        radar_collision = g_radar_collision;
        radar_off = g_radar_target_off;
        g_radar_target_on = 0;
        g_radar_collision = 0;
        g_radar_target_off = 0;
        pthread_mutex_unlock(&g_radar_mutex);

        /* 处理雷达事件 */
        if (radar_on && fusion.state == FUSION_IDLE) {
            printf("\n========================================\n");
            printf("[RADAR] >>> TARGET_ON - 目标出现, 启动 NPU 验证\n");
            printf("========================================\n\n");
            fusion.state = FUSION_TARGET_ON;
            fusion.confirm_count = 0;
            fusion.deny_count = 0;
            fusion.total_detections = 0;
            fusion.road_user_detections = 0;
            fusion.collision_received = 0;
            fusion.last_detected_label[0] = '\0';
            fusion.last_detected_score = 0;
            struct timeval tv;
            gettimeofday(&tv, NULL);
            fusion.target_on_time_us = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
        }

        if (radar_collision && fusion.state != FUSION_IDLE) {
            fusion.collision_received = 1;
            printf("\n[RADAR] >>> COLLISION - 碰撞风险!\n");
            if (fusion.state == FUSION_CONFIRMED) {
                printf("[FUSION] *** ALERT: COLLISION CONFIRMED by NPU - REAL ROAD USER ***\n");
            } else {
                printf("[FUSION] *** WARNING: COLLISION but NPU verification pending... ***\n");
            }
            printf("\n");
        }

        if (radar_off) {
            double elapsed = 0;
            if (fusion.target_on_time_us > 0) {
                struct timeval tv;
                gettimeofday(&tv, NULL);
                int64_t now_us = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
                elapsed = (now_us - fusion.target_on_time_us) / 1000000.0;
            }
            printf("\n========================================\n");
            printf("[RADAR] >>> TARGET_OFF - 目标消失\n");
            printf("[FUSION] 统计: 检测 %d 帧, 道路用户 %d 帧, 碰撞 %s\n",
                   fusion.total_detections, fusion.road_user_detections,
                   fusion.collision_received ? "是" : "否");
            printf("[FUSION] 最终判定: %s (耗时 %.1fs)\n",
                   fusion.state == FUSION_CONFIRMED ? "真实道路用户" :
                   fusion.state == FUSION_DENIED ? "雷达误触发" : "未确认",
                   elapsed);
            printf("========================================\n\n");
            fusion.state = FUSION_IDLE;
        }

        /* 捕获摄像头帧 */
        uint8_t *jpeg_buf;
        unsigned int jpeg_len;
        if (camera_capture(&cam, &jpeg_buf, &jpeg_len) < 0) break;
        frame_count++;

        /* JPEG 解码 */
        int dec_w, dec_h;
        if (jpeg_decode_rgb(jpeg_buf, jpeg_len, rgb_full, &dec_w, &dec_h) < 0) continue;

        /* 缩放 + NPU 推理 */
        resize_rgb(rgb_full, dec_w, dec_h, rgb_nn, nn_w, nn_h);
        frame_results_t results = detector.detect(rgb_nn);

        /* 每帧打印时间戳 */
        struct timeval tv;
        gettimeofday(&tv, NULL);
        double t = (tv.tv_sec - t_start.tv_sec) + (tv.tv_usec - t_start.tv_usec) / 1000000.0;

        if (fusion.state == FUSION_IDLE) {
            /* 空闲状态: 静默运行, 只打印心跳 */
            if (frame_count % 30 == 0) {
                printf("[%6.1fs] IDLE... (frame #%d, NPU: %.1fms)\n",
                       t, frame_count, results.inference_time_ms);
                fflush(stdout);
            }
        } else {
            /* 目标验证状态 */
            printf("[%6.1fs] Frame #%d | NPU: %.1fms | %zu objects:",
                   t, frame_count, results.inference_time_ms, results.objects.size());

            if (results.objects.empty()) {
                printf(" (none)\n");
            } else {
                printf("\n");
                for (size_t i = 0; i < results.objects.size(); i++) {
                    const detect_result_t &obj = results.objects[i];
                    const std::string &label = detector.get_label(obj.class_index);
                    int road = is_road_user(obj.class_index);
                    printf("  %s [%s] score=%.2f\n",
                           road ? "**ROAD USER**" : "  (ignore)  ",
                           label.c_str(), obj.score);
                }
            }

            /* 融合判定 */
            int has_road_user = 0;
            for (size_t i = 0; i < results.objects.size(); i++) {
                if (is_road_user(results.objects[i].class_index)) {
                    has_road_user = 1;
                    strncpy(fusion.last_detected_label,
                            detector.get_label(results.objects[i].class_index).c_str(),
                            sizeof(fusion.last_detected_label) - 1);
                    fusion.last_detected_score = results.objects[i].score;
                    break;
                }
            }

            fusion.total_detections++;
            if (has_road_user) {
                fusion.road_user_detections++;
                fusion.confirm_count++;
                fusion.deny_count = 0;
                if (fusion.confirm_count >= NPU_CONFIRM_NEEDED && fusion.state == FUSION_TARGET_ON) {
                    fusion.state = FUSION_CONFIRMED;
                    printf("[FUSION] *** CONFIRMED: Real road user detected (%s, score=%.2f) ***\n",
                           fusion.last_detected_label, fusion.last_detected_score);
                }
            } else {
                fusion.confirm_count = 0;
                fusion.deny_count++;
                if (fusion.deny_count >= NPU_DENY_NEEDED && fusion.state == FUSION_TARGET_ON) {
                    fusion.state = FUSION_DENIED;
                    printf("[FUSION] *** DENIED: Radar false trigger (no road user in %d frames) ***\n",
                           NPU_DENY_NEEDED);
                }
            }
            fflush(stdout);
        }
    }

    printf("\n[FUSION] Stopping...\n");

    pthread_cancel(pipe_thread);
    pthread_join(pipe_thread, NULL);

    camera_stop(&cam);
    camera_close(&cam);
    free(rgb_full);
    free(rgb_nn);
    unlink(PIPE_PATH);

    printf("[FUSION] Done. Total frames: %d\n", frame_count);
    return 0;
}