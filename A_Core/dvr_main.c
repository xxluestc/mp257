#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include "dvr_engine.h"

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
    printf("Options:\n");
    printf("  -d, --device     Camera device (default: /dev/video0)\n");
    printf("  -W, --width      Frame width (default: 640)\n");
    printf("  -H, --height     Frame height (default: 480)\n");
    printf("  -f, --fps        Frame rate (default: 30)\n");
    printf("  -s, --sd-path    SD card path (default: /run/media/mmcblk0p1)\n");
    printf("  -b, --buffer     Buffer seconds (default: 30)\n");
    printf("  -D, --display    Enable LCD display (0/1, default: 1)\n");
    printf("  -h, --help       Show this help\n");
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
    config.enable_display     = 1;
    config.display_mode       = DISPLAY_MODE_LCD;
    strcpy(config.camera_device, "/dev/video-camera0");
    strcpy(config.sd_card_path, "/run/media/mmcblk0p1");

    static struct option long_opts[] = {
        {"device",  required_argument, 0, 'd'},
        {"width",   required_argument, 0, 'W'},
        {"height",  required_argument, 0, 'H'},
        {"fps",     required_argument, 0, 'f'},
        {"sd-path", required_argument, 0, 's'},
        {"buffer",  required_argument, 0, 'b'},
        {"display", required_argument, 0, 'D'},
        {"help",    no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "d:W:H:f:s:b:D:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'd': strncpy(config.camera_device, optarg, DVR_MAX_PATH - 1); break;
        case 'W': config.width  = atoi(optarg); break;
        case 'H': config.height = atoi(optarg); break;
        case 'f': config.fps    = atoi(optarg); break;
        case 's': strncpy(config.sd_card_path, optarg, DVR_MAX_PATH - 1); break;
        case 'b': config.buffer_seconds = atoi(optarg); break;
        case 'D': config.enable_display = atoi(optarg); break;
        case 'h': print_usage(argv[0]); return 0;
        default:  print_usage(argv[0]); return 1;
        }
    }

    printf("========================================\n");
    printf("  DVR - Dashcam Video Recorder\n");
    printf("========================================\n");
    printf("  Camera:   %s\n", config.camera_device);
    printf("  Resolution: %dx%d @ %d fps\n", config.width, config.height, config.fps);
    printf("  SD Card:  %s\n", config.sd_card_path);
    printf("  Buffer:   %d seconds\n", config.buffer_seconds);
    printf("  Display:  %s\n", config.enable_display ? "ON" : "OFF");
    printf("  Trigger:  /tmp/dvr_trigger_pipe\n");
    printf("----------------------------------------\n");
    printf("  Commands via pipe:\n");
    printf("    TARGET_ON   - start buffering\n");
    printf("    TARGET_OFF  - stop buffering\n");
    printf("    WARNING     - emergency save\n");
    printf("    FALL        - emergency save\n");
    printf("    COLLISION   - emergency save\n");
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
