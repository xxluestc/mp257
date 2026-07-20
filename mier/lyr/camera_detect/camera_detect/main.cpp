/**
 * camera_detect - USB Camera Object Detection (background)
 *
 * Opens USB camera, captures MJPEG frames, decodes to RGB,
 * runs NPU inference (SSD MobileNet V2), prints detected objects.
 *
 * Usage: ./camera_detect [options]
 *   -d <device>    Video device (default: /dev/video6)
 *   -m <model>     NPU model path (default: models/ssd_mobilenet_v2_fpnlite_10_256_int8_per_tensor.nb)
 *   -l <labels>    Labels file path (default: models/labels_coco_dataset_80.txt)
 *   -c <conf>      Confidence threshold (default: 0.70)
 *   -w <width>     Camera width (default: 1280)
 *   -H <height>    Camera height (default: 720)
 *   -n <n>         Only run N inferences then exit (0 = infinite)
 *   -h             Show help
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/time.h>

#include "camera.h"
#include "npu_detect.h"

/* External JPEG decoder (libjpeg) */
extern "C" int jpeg_decode_rgb(const unsigned char *jpeg_data, unsigned long jpeg_size,
                                unsigned char *out_rgb, int *out_width, int *out_height);

static volatile int g_running = 1;

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/**
 * @brief 打印命令行使用说明
 * @param prog 程序名
 */
static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -d <device>   Video device (default: /dev/video6)\n");
    printf("  -m <model>    NPU model .nb file\n");
    printf("  -l <labels>   Labels .txt file\n");
    printf("  -c <conf>     Confidence threshold (default: 0.70)\n");
    printf("  -w <width>    Camera width (default: 1280)\n");
    printf("  -H <height>   Camera height (default: 720)\n");
    printf("  -n <n>        Run N inferences then exit (0=infinite)\n");
    printf("  -h            Show this help\n");
}

/* Simple nearest-neighbor resize RGB image */
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

int main(int argc, char *argv[]) {
    const char *device = "/dev/video6";
    const char *model_path = "models/ssd_mobilenet_v2_fpnlite_10_256_int8_per_tensor.nb";
    const char *labels_path = "models/labels_coco_dataset_80.txt";
    float confidence = 0.70f;
    int cam_width = 1280, cam_height = 720;
    int max_inferences = 0; /* 0 = infinite */

    /* Parse arguments */
    int opt;
    while ((opt = getopt(argc, argv, "d:m:l:c:w:H:n:h")) != -1) {
        switch (opt) {
        case 'd': device = optarg; break;
        case 'm': model_path = optarg; break;
        case 'l': labels_path = optarg; break;
        case 'c': confidence = atof(optarg); break;
        case 'w': cam_width = atoi(optarg); break;
        case 'H': cam_height = atoi(optarg); break;
        case 'n': max_inferences = atoi(optarg); break;
        case 'h':
        default: print_usage(argv[0]); return 0;
        }
    }

    printf("========================================\n");
    printf(" Camera Object Detection (NPU)\n");
    printf("========================================\n");
    printf("Device:   %s\n", device);
    printf("Model:    %s\n", model_path);
    printf("Labels:   %s\n", labels_path);
    printf("Conf:     %.2f\n", confidence);
    printf("Camera:   %dx%d\n", cam_width, cam_height);
    printf("========================================\n");

    /* 1. 初始化 NPU 检测器：加载模型、读取标签、分配输入张量 */
    NpuDetector detector(model_path, labels_path, confidence, 0.45f);
    int nn_w = detector.get_input_width();
    int nn_h = detector.get_input_height();
    printf("[MAIN] NN input size: %dx%d\n", nn_w, nn_h);

    /* 2. 打开摄像头并协商格式/帧率 */
    camera_t cam;
    if (camera_open(&cam, device, cam_width, cam_height) < 0) {
        fprintf(stderr, "[MAIN] Failed to open camera\n");
        return 1;
    }

    /* 3. 分配解码缓冲区与模型输入缓冲区 */
    int rgb_size = cam.width * cam.height * 3;
    uint8_t *rgb_full = (uint8_t *)malloc(rgb_size);
    int nn_size = nn_w * nn_h * 3;
    uint8_t *rgb_nn = (uint8_t *)malloc(nn_size);

    if (!rgb_full || !rgb_nn) {
        fprintf(stderr, "[MAIN] Failed to allocate buffers\n");
        camera_close(&cam);
        return 1;
    }

    /* 4. 启动视频流 */
    if (camera_start(&cam) < 0) {
        fprintf(stderr, "[MAIN] Failed to start camera\n");
        camera_close(&cam);
        return 1;
    }
    printf("[MAIN] Camera stream started\n"); fflush(stdout);

    /* 5. 注册信号处理，支持 Ctrl+C 优雅退出 */
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("[MAIN] Running... Press Ctrl+C to stop.\n\n");
    fflush(stdout);

    int frame_count = 0;
    int detect_count = 0;
    struct timeval t_start, t_now;
    gettimeofday(&t_start, nullptr);

    /* 6. 主循环：采集 -> 解码 -> resize -> NPU 推理 -> 输出结果 */
    while (g_running && (max_inferences == 0 || detect_count < max_inferences)) {
        /* 6.1 从摄像头取出一帧 MJPEG */
        uint8_t *jpeg_buf;
        unsigned int jpeg_len;
        if (camera_capture(&cam, &jpeg_buf, &jpeg_len) < 0) {
            fprintf(stderr, "[MAIN] Capture failed\n");
            break;
        }
        frame_count++;

        /* 6.2 解码 JPEG 到 RGB24 */
        int dec_w, dec_h;
        if (jpeg_decode_rgb(jpeg_buf, jpeg_len, rgb_full, &dec_w, &dec_h) < 0) {
            fprintf(stderr, "[MAIN] JPEG decode failed\n");
            continue;
        }

        /* 6.3 resize 到模型输入尺寸 */
        resize_rgb(rgb_full, dec_w, dec_h, rgb_nn, nn_w, nn_h);

        /* 6.4 NPU 推理 */
        frame_results_t results = detector.detect(rgb_nn);
        detect_count++;

        /* 6.5 打印检测结果 */
        gettimeofday(&t_now, nullptr);
        double elapsed = (t_now.tv_sec - t_start.tv_sec) +
                         (t_now.tv_usec - t_start.tv_usec) / 1000000.0;

        printf("[%6.1fs] Frame #%d (detect #%d) | NPU: %.1fms | %zu objects",
               elapsed, frame_count, detect_count,
               results.inference_time_ms, results.objects.size());

        if (results.objects.empty()) {
            printf(" | (none)\n");
        } else {
            printf("\n");
            for (size_t i = 0; i < results.objects.size(); i++) {
                const detect_result_t &obj = results.objects[i];
                const std::string &label = detector.get_label(obj.class_index);
                printf("  -> [%s] score=%.2f  box=(%.2f,%.2f)-(%.2f,%.2f)\n",
                       label.c_str(), obj.score,
                       obj.x0, obj.y0, obj.x1, obj.y1);
            }
        }
        fflush(stdout);
    }

    printf("\n[MAIN] Stopping. Total: %d frames, %d detections\n", frame_count, detect_count);

    camera_stop(&cam);
    camera_close(&cam);
    free(rgb_full);
    free(rgb_nn);

    printf("[MAIN] Done.\n");
    return 0;
}