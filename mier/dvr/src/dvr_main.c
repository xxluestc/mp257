/**
 * dvr_main.c - DVR 行车记录主入口
 *
 * 使用方法:
 *   手动模式 (命名管道触发):
 *     ./dvr -d /dev/video6 -s /tmp -W 1280 -H 720 -f 25
 *     echo "TARGET_ON" > /tmp/dvr_trigger_pipe
 *
 *   NPU融合模式 (摄像头AI验证雷达目标):
 *     ./dvr -d /dev/video6 -s /run/media/mmcblk0p1/dvr \
 *           --npu-model /usr/local/share/npu/ssd_mobilenet_v2_fpnlite_10_256_int8_per_tensor.nb \
 *           --npu-labels /usr/local/share/npu/labels_coco_dataset_80.txt
 *
 *   自动模式 (--auto 自动模拟完整流程):
 *     ./dvr --auto --target-delay 5 --collision-delay 25
 *
 * 触发命令 (通过命名管道):
 *   echo "TARGET_ON"  > /tmp/dvr_trigger_pipe   # 开始录制
 *   echo "WARNING"    > /tmp/dvr_trigger_pipe   # 触发保存(前后各15s)
 *   echo "TARGET_OFF" > /tmp/dvr_trigger_pipe   # 停止录制
 */

#include "dvr_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>

static dvr_engine_t *g_engine = NULL;

static void signal_handler(int sig)
{
    (void)sig;
    printf("\n[DVR-MAIN] Signal %d received, shutting down...\n", sig);
    if (g_engine) dvr_engine_stop(g_engine);
}

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("\nOptions:\n");
    printf("  -d, --device       Camera device (default: /dev/video6)\n");
    printf("  -s, --storage      Storage path (default: /run/media/mmcblk0p1/dvr)\n");
    printf("  -W, --width        Frame width (default: 1280)\n");
    printf("  -H, --height       Frame height (default: 720)\n");
    printf("  -f, --fps          Frame rate (default: 25)\n");
    printf("  -b, --buffer       Buffer seconds (default: 38)\n");
    printf("\nNPU fusion options:\n");
    printf("  --npu-model PATH   NPU model file (.nb) for radar target verification\n");
    printf("  --npu-labels PATH  Labels file for the model\n");
    printf("  --npu-confidence F Confidence threshold (default: 0.6)\n");
    printf("\nAuto-trigger mode:\n");
    printf("  --auto             Enable auto-trigger mode (no pipe needed)\n");
    printf("  --target-delay N   Seconds before TARGET_ON (default: 3)\n");
    printf("  --collision-delay N Seconds after TARGET_ON before COLLISION (default: 25)\n");
    printf("  --auto-event TYPE  Event type: warning | collision (default: collision)\n");
    printf("\nManual mode (named pipe):\n");
    printf("  echo \"TARGET_ON\"  > /tmp/dvr_trigger_pipe\n");
    printf("  echo \"TARGET_OFF\" > /tmp/dvr_trigger_pipe\n");
    printf("  echo \"WARNING\"    > /tmp/dvr_trigger_pipe\n");
    printf("  echo \"FALL\"       > /tmp/dvr_trigger_pipe\n");
    printf("  echo \"COLLISION\"  > /tmp/dvr_trigger_pipe\n");
    printf("\nAuto mode example:\n");
    printf("  %s --auto --target-delay 5 --collision-delay 25\n", prog);
    printf("  (starts recording at 5s, triggers collision at 30s, saves 15s before+after)\n");
    printf("  -h, --help         Show this help\n");
}

int main(int argc, char *argv[])
{
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    dvr_config_t config;
    memset(&config, 0, sizeof(config));

    config.width              = DVR_DEFAULT_WIDTH;
    config.height             = DVR_DEFAULT_HEIGHT;
    config.fps                = DVR_DEFAULT_FPS;
    config.buffer_seconds     = DVR_BUFFER_SECONDS;
    config.save_before_seconds = DVR_SAVE_BEFORE_SEC;
    config.save_after_seconds  = DVR_SAVE_AFTER_SEC;
    config.auto_mode           = 0;
    config.auto_target_delay   = 3;
    config.auto_collision_delay = 25;
    config.auto_event          = AUTO_EVENT_COLLISION;
    strcpy(config.camera_device, "/dev/video6");
    strcpy(config.storage_path, "/run/media/mmcblk0p1/dvr");
    config.npu_model_path[0]   = '\0';
    config.npu_labels_path[0]  = '\0';
    config.npu_confidence       = 0.6f;
    config.npu_enabled          = 0;

    static struct option long_opts[] = {
        {"device",         required_argument, 0, 'd'},
        {"storage",        required_argument, 0, 's'},
        {"width",          required_argument, 0, 'W'},
        {"height",         required_argument, 0, 'H'},
        {"fps",            required_argument, 0, 'f'},
        {"buffer",         required_argument, 0, 'b'},
        {"auto",           no_argument,       0, 'A'},
        {"target-delay",   required_argument, 0, 'T'},
        {"collision-delay",required_argument, 0, 'C'},
        {"auto-event",     required_argument, 0, 'E'},
        {"npu-model",      required_argument, 0, 'N'},
        {"npu-labels",     required_argument, 0, 'L'},
        {"npu-confidence", required_argument, 0, 'F'},
        {"help",           no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "d:s:W:H:f:b:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'd': strncpy(config.camera_device, optarg, DVR_MAX_PATH - 1); break;
        case 's': strncpy(config.storage_path,  optarg, DVR_MAX_PATH - 1); break;
        case 'W': config.width  = atoi(optarg); break;
        case 'H': config.height = atoi(optarg); break;
        case 'f': config.fps    = atoi(optarg); break;
        case 'b': config.buffer_seconds = atoi(optarg); break;
        case 'A': config.auto_mode = 1; break;
        case 'T': config.auto_target_delay = atoi(optarg); break;
        case 'C': config.auto_collision_delay = atoi(optarg); break;
        case 'E':
            if (strcmp(optarg, "warning") == 0 || strcmp(optarg, "WARNING") == 0)
                config.auto_event = AUTO_EVENT_WARNING;
            else
                config.auto_event = AUTO_EVENT_COLLISION;
            break;
        case 'N':
            strncpy(config.npu_model_path, optarg, DVR_MAX_PATH - 1);
            config.npu_enabled = 1;
            break;
        case 'L':
            strncpy(config.npu_labels_path, optarg, DVR_MAX_PATH - 1);
            break;
        case 'F':
            config.npu_confidence = (float)atof(optarg);
            break;
        case 'h': print_usage(argv[0]); return 0;
        default:  print_usage(argv[0]); return 1;
        }
    }

    printf("========================================\n");
    printf("  DVR - Dashcam Video Recorder (USB)\n");
    printf("========================================\n");
    printf("  Camera:   %s\n", config.camera_device);
    printf("  Resolution: %dx%d @ %d fps\n", config.width, config.height, config.fps);
    printf("  Storage:  %s\n", config.storage_path);
    printf("  Buffer:   %d seconds\n", config.buffer_seconds);
    printf("  Save:     %ds before + %ds after trigger\n",
           config.save_before_seconds, config.save_after_seconds);

    if (config.auto_mode) {
        printf("\n  *** AUTO-TRIGGER MODE ***\n");
        printf("  TARGET_ON  after %ds\n", config.auto_target_delay);
        printf("  COLLISION  after %ds (total %ds from start)\n",
               config.auto_collision_delay,
               config.auto_target_delay + config.auto_collision_delay);
        printf("  Event:     %s\n",
               config.auto_event == AUTO_EVENT_WARNING ? "WARNING" : "COLLISION");
        if (config.auto_collision_delay < config.save_before_seconds) {
            printf("  WARNING: collision-delay (%d) < save-before (%d), buffer may be incomplete!\n",
                   config.auto_collision_delay, config.save_before_seconds);
        }
    } else {
        printf("  Trigger:  /tmp/dvr_trigger_pipe\n");
        printf("\n  Commands via pipe:\n");
        printf("    TARGET_ON   - start buffering\n");
        printf("    TARGET_OFF  - stop buffering\n");
        printf("    WARNING     - emergency save\n");
        printf("    FALL        - emergency save (protected)\n");
        printf("    COLLISION   - emergency save (protected)\n");
    }

    if (config.npu_enabled && config.npu_model_path[0]) {
        printf("\n  *** NPU FUSION ENABLED ***\n");
        printf("  Model:  %s\n", config.npu_model_path);
        printf("  Labels: %s\n", config.npu_labels_path);
        printf("  Confidence: %.2f\n", (double)config.npu_confidence);
        printf("  Road users: person, bicycle, car, motorcycle, bus, truck\n");
        printf("  Rule: COLLISION only saved if camera confirms road user\n");
    }
    printf("========================================\n\n");

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    g_engine = dvr_engine_create(&config);
    if (!g_engine) {
        fprintf(stderr, "[DVR-MAIN] Failed to create engine\n");
        return 1;
    }

    dvr_engine_run(g_engine);
    dvr_engine_destroy(g_engine);

    printf("[DVR-MAIN] Exit\n");
    return 0;
}