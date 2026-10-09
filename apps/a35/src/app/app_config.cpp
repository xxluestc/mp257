#include "app/services.hpp"
#include "runtime/video_pipeline.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <climits>
#include <limits>

namespace {
int parse_integer(const char *value) {
    errno = 0;
    char *end = nullptr;
    long parsed = strtol(value, &end, 10);
    if (errno || end == value || *end || parsed < INT_MIN || parsed > INT_MAX)
        return INT_MIN;
    return static_cast<int>(parsed);
}

float parse_number(const char *value) {
    errno = 0;
    char *end = nullptr;
    float parsed = strtof(value, &end);
    if (errno || end == value || *end || !std::isfinite(parsed))
        return std::numeric_limits<float>::quiet_NaN();
    return parsed;
}

bool copy_option(char *destination, size_t capacity, const char *value) {
    if (!value[0] || strlen(value) >= capacity) {
        fprintf(stderr, "Option is empty or too long\n");
        return false;
    }
    strcpy(destination, value);
    return true;
}
} // namespace

int parse_arguments(int argc, char **argv, helmet::VideoConfig &video, std::string &radar_device) {
    const char *camera_dev = "/dev/video7";
    const char *uart_dev = UART_DEVICE;
    const char *model_path = "models/ssd_mobilenet_v2_fpnlite_10_256_int8_per_tensor.nb";
    const char *labels_path = "models/labels_coco_dataset_80.txt";
    float confidence = 0.60f;
    int pool_mib = 224, jpeg_kib = 256;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            camera_dev = argv[++i];
        else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc)
            uart_dev = argv[++i];
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
            confidence = parse_number(argv[++i]);
        else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc)
            model_path = argv[++i];
        else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc)
            labels_path = argv[++i];
        else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
            g_test_fall_delay_sec = parse_integer(argv[++i]);
        else if (strcmp(argv[i], "--test-fall-count") == 0 && i + 1 < argc)
            g_test_fall_count = parse_integer(argv[++i]);
        else if (strcmp(argv[i], "--test-fall-interval") == 0 && i + 1 < argc)
            g_test_fall_interval_sec = parse_integer(argv[++i]);
        else if (strcmp(argv[i], "-V") == 0 && i + 1 < argc)
            g_test_v2x_delay_sec = parse_integer(argv[++i]);
        else if (strcmp(argv[i], "-x") == 0 && i + 1 < argc) {
            if (!copy_option(g_test_v2x_direction, sizeof(g_test_v2x_direction), argv[++i]))
                return 2;
        } else if (strcmp(argv[i], "-T") == 0 && i + 1 < argc)
            g_ttc_threshold = parse_number(argv[++i]);
        else if (strcmp(argv[i], "-D") == 0 && i + 1 < argc)
            g_dist_threshold = parse_number(argv[++i]);
        else if (strcmp(argv[i], "--left-angle") == 0 && i + 1 < argc)
            g_angle_left_threshold = parse_number(argv[++i]);
        else if (strcmp(argv[i], "--right-angle") == 0 && i + 1 < argc)
            g_angle_right_threshold = parse_number(argv[++i]);
        else if (strcmp(argv[i], "--angle-sign") == 0 && i + 1 < argc)
            g_angle_direction_sign = parse_number(argv[++i]);
        else if (strcmp(argv[i], "--angle-alpha") == 0 && i + 1 < argc)
            g_angle_filter_alpha = parse_number(argv[++i]);
        else if (strcmp(argv[i], "--direction-samples") == 0 && i + 1 < argc)
            g_direction_stable_samples = parse_integer(argv[++i]);
        else if (strcmp(argv[i], "--radar-log-dir") == 0 && i + 1 < argc) {
            if (!copy_option(g_radar_log_dir, sizeof(g_radar_log_dir), argv[++i]))
                return 2;
        } else if (strcmp(argv[i], "--dvr-dir") == 0 && i + 1 < argc) {
            const char *value = argv[++i];
            if (strlen(value) >= sizeof(g_dvr_base_dir)) {
                fprintf(stderr, "[DVR] dvr-dir is too long\n");
                return 2;
            }
            strcpy(g_dvr_base_dir, value);
        } else if (strcmp(argv[i], "--dvr-mount-dir") == 0 && i + 1 < argc) {
            const char *value = argv[++i];
            if (strlen(value) >= sizeof(g_dvr_mount_dir)) {
                fprintf(stderr, "[DVR] dvr-mount-dir is too long\n");
                return 2;
            }
            strcpy(g_dvr_mount_dir, value);
        } else if (strcmp(argv[i], "--ble-led-uart") == 0 && i + 1 < argc) {
            if (!copy_option(g_ble_led_uart, sizeof(g_ble_led_uart), argv[++i]))
                return 2;
        } else if (strcmp(argv[i], "--no-ble-led") == 0)
            g_ble_led_enabled = false;
        else if (strcmp(argv[i], "--pool-mib") == 0 && i + 1 < argc)
            pool_mib = parse_integer(argv[++i]);
        else if (strcmp(argv[i], "--max-jpeg-kib") == 0 && i + 1 < argc)
            jpeg_kib = parse_integer(argv[++i]);
        else if (strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [-d camera] [-u uart] [-c conf] [-T ttc] [-D dist]\n"
                   "          [--left-angle deg] [--right-angle deg]\n"
                   "          [--angle-sign -1|1]\n"
                   "          [--angle-alpha 0..1] [--direction-samples n]\n"
                   "          [--radar-log-dir path] [--pool-mib 224] [--max-jpeg-kib 256]\n"
                   "          [--dvr-dir path] [--dvr-mount-dir path]\n"
                   "          [--ble-led-uart path] [--no-ble-led]\n"
                   "          [-t fall_delay] [--test-fall-count n]\n"
                   "          [--test-fall-interval sec]\n"
                   "          [-V v2x_delay] [-x v2x_dir] [-h]\n",
                   argv[0]);
            printf("  v2x_dir: nearby|left_front|right_front|left|right\n");
            printf("  Direction uses rider_angle = sensor_angle * angle_sign\n");
            printf("  rider_angle <= left is LEFT; rider_angle >= right is RIGHT\n");
            printf("  Defaults: TTC=%.1fs distance=%.1fm left=%.1fdeg right=%.1fdeg\n",
                   TTC_THRESHOLD_DEFAULT, (float)DIST_THRESHOLD_DEFAULT, ANGLE_LEFT_DEFAULT,
                   ANGLE_RIGHT_DEFAULT);
            return 1;
        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]);
            return 2;
        }
    }

    if (g_ttc_threshold <= 0.0f || g_dist_threshold <= 0.0f) {
        fprintf(stderr, "[RADAR] TTC and distance thresholds must be positive\n");
        return 2;
    }
    if (g_test_fall_delay_sec < 0 || g_test_fall_delay_sec > 86400 || g_test_v2x_delay_sec < 0 ||
        g_test_v2x_delay_sec > 86400 || g_test_fall_count < 1 || g_test_fall_count > 20 ||
        g_test_fall_interval_sec < 1) {
        fprintf(stderr, "[TEST] fall count must be 1..20 and interval positive\n");
        return 2;
    }
    if (g_angle_left_threshold >= g_angle_right_threshold) {
        fprintf(stderr, "[RADAR] left-angle must be smaller than right-angle\n");
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
    if (g_direction_stable_samples < 1 || g_direction_stable_samples > 20) {
        fprintf(stderr, "[RADAR] direction-samples must be between 1 and 20\n");
        return 2;
    }
    if (finalize_dvr_storage_paths() != 0)
        return 2;

    if (!std::isfinite(confidence) || confidence <= 0 || confidence > 1 ||
        !std::isfinite(g_ttc_threshold) || !std::isfinite(g_dist_threshold) ||
        !std::isfinite(g_angle_left_threshold) || !std::isfinite(g_angle_right_threshold) ||
        !std::isfinite(g_angle_filter_alpha) || !std::isfinite(g_angle_direction_sign) ||
        pool_mib < 64 || pool_mib > 512 || jpeg_kib < 64 || jpeg_kib > 1024) {
        fprintf(stderr, "Invalid confidence, numeric threshold or RAM budget\n");
        return 2;
    }
    video.camera_device = camera_dev;
    video.model_path = model_path;
    video.labels_path = labels_path;
    video.confidence = confidence;
    video.output_directory = g_dvr_base_dir;
    video.mount_directory = g_dvr_mount_dir;
    video.max_jpeg_bytes = static_cast<size_t>(jpeg_kib) * 1024;
    video.pool_slots = static_cast<size_t>(pool_mib) * 1024 * 1024 / video.max_jpeg_bytes;
    try {
        helmet::validate_video_config(video);
    } catch (const std::invalid_argument &error) {
        fprintf(stderr, "Invalid video configuration: %s\n", error.what());
        return 2;
    }
    radar_device = uart_dev;
    return 0;
}
