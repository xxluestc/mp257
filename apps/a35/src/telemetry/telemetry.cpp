#include "app/services.hpp"
#include "runtime/video_config.hpp"
#include "runtime/linux_resources.hpp"
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
#include <ctime>
#include <cmath>
#include <limits.h>
#include <mutex>
#include <fcntl.h>

static FILE *g_radar_csv = NULL;
static FILE *g_sensor_csv = NULL;
static std::mutex g_sensor_csv_mutex;
static char g_radar_csv_path[PATH_MAX];
static char g_sensor_csv_path[PATH_MAX];
static char g_radar_state_path[PATH_MAX];
static unsigned int g_radar_csv_write_count = 0;
static unsigned int g_sensor_csv_write_count = 0;

/* 业务存储尚未就绪时，关键事件先进入有界内存队列，就绪后按原顺序补写。 */
#define SENSOR_PENDING_MAX 128U
#define SENSOR_PENDING_LINE_MAX 1536U
static char g_sensor_pending[SENSOR_PENDING_MAX][SENSOR_PENDING_LINE_MAX];
static unsigned int g_sensor_pending_head = 0;
static unsigned int g_sensor_pending_count = 0;
static unsigned int g_sensor_pending_dropped = 0;

#define TELEMETRY_LOG_MAX_BYTES (20U * 1024U * 1024U)
#define TELEMETRY_LOG_BACKUPS 4
#define TELEMETRY_SIZE_CHECK_WRITES 256U

static int mkdir_recursive(const char *path) {
    if (path == NULL || path[0] == '\0')
        return -1;
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp))
        return -1;

    size_t len = strlen(tmp);
    if (len > 1 && tmp[len - 1] == '/')
        tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p != '\0'; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, 0775) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0775) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/*
 * 写入者在调用前必须先关闭文件。采用同目录 rename，避免复制大 CSV；
 * 当前文件加四份历史文件，总容量上限约为 100 MiB/日志类型。
 */
static int rotate_numbered_file(const char *path, int backups) {
    if (path == NULL || path[0] == '\0' || backups <= 0)
        return -1;

    char source[PATH_MAX];
    char destination[PATH_MAX];
    for (int index = backups; index >= 1; index--) {
        if (index == 1) {
            if (snprintf(source, sizeof(source), "%s", path) >= (int)sizeof(source))
                return -1;
        } else {
            if (snprintf(source, sizeof(source), "%s.%d", path, index - 1) >= (int)sizeof(source))
                return -1;
        }
        if (snprintf(destination, sizeof(destination), "%s.%d", path, index) >=
            (int)sizeof(destination))
            return -1;

        if (index == backups)
            unlink(destination);
        if (rename(source, destination) != 0 && errno != ENOENT) {
            fprintf(stderr, "[LOG_ROTATE] rename %s -> %s failed: %s\n", source, destination,
                    strerror(errno));
            return -1;
        }
    }
    return 0;
}

static FILE *open_file(const char *path, const char *mode, int flags) {
    helmet::UniqueFd descriptor(open(path, flags | O_CLOEXEC, 0644));
    if (descriptor.get() < 0)
        return nullptr;
    FILE *file = fdopen(descriptor.get(), mode);
    if (file)
        descriptor.release(); // FILE owns the fd from here through fclose().
    return file;
}

static FILE *open_csv_append(const char *path, const char *header) {
    struct stat st;
    int needs_header = (stat(path, &st) != 0 || st.st_size == 0);
    FILE *fp = open_file(path, "a", O_WRONLY | O_CREAT | O_APPEND);
    if (fp == NULL)
        return NULL;
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
    if (fp == NULL)
        return 0;
    if (fflush(fp) != 0 || fstat(fileno(fp), &st) != 0)
        return 0;
    return st.st_size >= (off_t)TELEMETRY_LOG_MAX_BYTES;
}

static void format_timestamp_iso(const struct timeval *tv, char *out, size_t out_size) {
    struct tm tm_utc;
    time_t seconds = tv->tv_sec;
    gmtime_r(&seconds, &tm_utc);
    char date[32];
    strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &tm_utc);
    int milliseconds = (int)(tv->tv_usec / 1000L);
    snprintf(out, out_size, "%s.%03dZ", date, milliseconds);
}

static void csv_sanitize(const char *input, char *output, size_t output_size) {
    if (output_size == 0)
        return;
    size_t used = 0;
    if (input != NULL) {
        for (const char *p = input; *p != '\0' && used + 1 < output_size; p++) {
            char ch = *p;
            if (ch == ',' || ch == '\r' || ch == '\n' || ch == '"')
                ch = ' ';
            output[used++] = ch;
        }
    }
    output[used] = '\0';
}

static const char SENSOR_CSV_HEADER[] = "timestamp,timestamp_ms,source,event_type,status,event_id,"
                                        "label,score,count,seq,reason,details\n";

static const char RADAR_CSV_HEADER[] = "timestamp,timestamp_ms,objId,distance_m,velocity_mps,"
                                       "angle_deg,filtered_angle_deg,TTC_s,direction,"
                                       "dangerous_objId,is_current_dangerous,radar_alert\n";

static void sensor_csv_maybe_rotate_locked(void) {
    g_sensor_csv_write_count++;
    if (g_sensor_csv == NULL || g_sensor_csv_write_count % TELEMETRY_SIZE_CHECK_WRITES != 0 ||
        !csv_needs_rotation(g_sensor_csv))
        return;

    fclose(g_sensor_csv);
    g_sensor_csv = NULL;
    if (rotate_numbered_file(g_sensor_csv_path, TELEMETRY_LOG_BACKUPS) != 0) {
        fprintf(stderr, "[SENSOR_DATA] Rotation failed for %s\n", g_sensor_csv_path);
    }
    g_sensor_csv = open_csv_append(g_sensor_csv_path, SENSOR_CSV_HEADER);
    g_sensor_csv_write_count = 0;
    if (g_sensor_csv == NULL) {
        fprintf(stderr, "[SENSOR_DATA] Reopen failed for %s: %s\n", g_sensor_csv_path,
                strerror(errno));
    } else {
        printf("[系统] [SENSOR_DATA] Rotated at %u MiB (keep=%d)\n",
               TELEMETRY_LOG_MAX_BYTES / (1024U * 1024U), TELEMETRY_LOG_BACKUPS);
    }
}

static int sensor_telemetry_init(void) {
    std::lock_guard<std::mutex> lock(g_sensor_csv_mutex);
    if (g_sensor_csv != NULL)
        return 0;
    if (mkdir_recursive(g_radar_log_dir) != 0)
        return -1;
    if (snprintf(g_sensor_csv_path, sizeof(g_sensor_csv_path), "%s/sensor_events.csv",
                 g_radar_log_dir) >= (int)sizeof(g_sensor_csv_path))
        return -1;

    struct stat st;
    if (stat(g_sensor_csv_path, &st) == 0 && st.st_size >= (off_t)TELEMETRY_LOG_MAX_BYTES)
        rotate_numbered_file(g_sensor_csv_path, TELEMETRY_LOG_BACKUPS);

    g_sensor_csv = open_csv_append(g_sensor_csv_path, SENSOR_CSV_HEADER);
    if (g_sensor_csv == NULL) {
        fprintf(stderr, "[SENSOR_DATA] Cannot open %s: %s\n", g_sensor_csv_path, strerror(errno));
        return -1;
    }
    g_sensor_csv_write_count = 0;
    unsigned int buffered = g_sensor_pending_count;
    for (unsigned int i = 0; i < g_sensor_pending_count; i++) {
        unsigned int index = (g_sensor_pending_head + i) % SENSOR_PENDING_MAX;
        fputs(g_sensor_pending[index], g_sensor_csv);
    }
    g_sensor_pending_head = 0;
    g_sensor_pending_count = 0;
    printf("[系统] [SENSOR_DATA] CSV: %s (20 MiB x current+4)\n", g_sensor_csv_path);
    if (buffered > 0 || g_sensor_pending_dropped > 0) {
        printf("[系统] [SENSOR_DATA] Flushed %u boot events from RAM"
               " (dropped=%u)\n",
               buffered, g_sensor_pending_dropped);
    }
    return 0;
}

void sensor_event_log(const char *source, const char *event_type, const char *status,
                      const char *event_id, const char *label, float score, int count, int seq,
                      const char *reason, const char *details) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t timestamp_ms = (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
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

    char score_text[32] = "";
    char count_text[32] = "";
    char seq_text[32] = "";
    if (score >= 0.0f)
        snprintf(score_text, sizeof(score_text), "%.4f", score);
    if (count >= 0)
        snprintf(count_text, sizeof(count_text), "%d", count);
    if (seq >= 0)
        snprintf(seq_text, sizeof(seq_text), "%d", seq);

    char line[SENSOR_PENDING_LINE_MAX];
    snprintf(line, sizeof(line), "%s,%llu,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n", timestamp,
             (unsigned long long)timestamp_ms, safe_source, safe_type, safe_status, safe_id,
             safe_label, score_text, count_text, seq_text, safe_reason, safe_details);

    std::lock_guard<std::mutex> lock(g_sensor_csv_mutex);
    if (g_sensor_csv == NULL) {
        if (g_sensor_pending_count == SENSOR_PENDING_MAX) {
            g_sensor_pending_head = (g_sensor_pending_head + 1) % SENSOR_PENDING_MAX;
            g_sensor_pending_count--;
            g_sensor_pending_dropped++;
        }
        unsigned int index = (g_sensor_pending_head + g_sensor_pending_count) % SENSOR_PENDING_MAX;
        snprintf(g_sensor_pending[index], SENSOR_PENDING_LINE_MAX, "%s", line);
        g_sensor_pending_count++;
        return;
    }
    fputs(line, g_sensor_csv);
    sensor_csv_maybe_rotate_locked();
}

void sensor_telemetry_close(void) {
    std::lock_guard<std::mutex> lock(g_sensor_csv_mutex);
    if (g_sensor_csv != NULL) {
        fclose(g_sensor_csv);
        g_sensor_csv = NULL;
    }
}

static bool sensor_telemetry_available() {
    std::lock_guard<std::mutex> lock(g_sensor_csv_mutex);
    return g_sensor_csv != NULL;
}

static int radar_telemetry_init(void) {
    if (mkdir_recursive(g_radar_log_dir) != 0) {
        fprintf(stderr, "[RADAR_DATA] Cannot create %s: %s\n", g_radar_log_dir, strerror(errno));
        return -1;
    }

    if (snprintf(g_radar_csv_path, sizeof(g_radar_csv_path), "%s/radar_data.csv",
                 g_radar_log_dir) >= (int)sizeof(g_radar_csv_path) ||
        snprintf(g_radar_state_path, sizeof(g_radar_state_path), "%s/radar_state.json",
                 g_radar_log_dir) >= (int)sizeof(g_radar_state_path)) {
        fprintf(stderr, "[RADAR_DATA] Log path is too long\n");
        return -1;
    }

    struct stat st;
    if (stat(g_radar_csv_path, &st) == 0 && st.st_size >= (off_t)TELEMETRY_LOG_MAX_BYTES)
        rotate_numbered_file(g_radar_csv_path, TELEMETRY_LOG_BACKUPS);

    g_radar_csv = open_csv_append(g_radar_csv_path, RADAR_CSV_HEADER);
    if (g_radar_csv == NULL) {
        fprintf(stderr, "[RADAR_DATA] Cannot open %s: %s\n", g_radar_csv_path, strerror(errno));
        return -1;
    }
    g_radar_csv_write_count = 0;
    printf("[系统] [RADAR_DATA] CSV: %s (20 MiB x current+4)\n", g_radar_csv_path);
    printf("[系统] [RADAR_DATA] Dashboard state: %s\n", g_radar_state_path);
    return 0;
}

static void json_write_float_or_null(FILE *fp, float value) {
    if (isfinite(value) && value >= 0.0f)
        fprintf(fp, "%.3f", value);
    else
        fputs("null", fp);
}

void radar_telemetry_publish(const radar_result_t *radar, int fusion_alert, bool available) {
    if (radar == NULL)
        return;

    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);
    uint64_t timestamp_ms = (uint64_t)tv_now.tv_sec * 1000ULL + (uint64_t)tv_now.tv_usec / 1000ULL;
    char timestamp[40];
    format_timestamp_iso(&tv_now, timestamp, sizeof(timestamp));

    if (g_radar_csv != NULL) {
        for (int i = 0; i < radar->obj_count; i++) {
            const radar_target_t *target = &radar->targets[i];
            fprintf(g_radar_csv, "%s,%llu,%d,%.3f,%.3f,%.3f,%.3f,", timestamp,
                    (unsigned long long)timestamp_ms, target->obj_id, target->distance,
                    target->velocity, target->angle, target->filtered_angle);
            if (target->ttc >= 0.0f)
                fprintf(g_radar_csv, "%.3f", target->ttc);
            fprintf(g_radar_csv, ",%s,%d,%d,%d\n", radar_direction_name(target->direction),
                    radar->dangerous_obj_id, i == radar->dangerous_index ? 1 : 0,
                    radar->should_alert ? 1 : 0);
        }
        g_radar_csv_write_count += (unsigned int)radar->obj_count;
        if (g_radar_csv_write_count % TELEMETRY_SIZE_CHECK_WRITES <
                (unsigned int)radar->obj_count &&
            csv_needs_rotation(g_radar_csv)) {
            fclose(g_radar_csv);
            g_radar_csv = NULL;
            if (rotate_numbered_file(g_radar_csv_path, TELEMETRY_LOG_BACKUPS) != 0) {
                fprintf(stderr, "[RADAR_DATA] Rotation failed for %s\n", g_radar_csv_path);
            }
            g_radar_csv = open_csv_append(g_radar_csv_path, RADAR_CSV_HEADER);
            g_radar_csv_write_count = 0;
            if (g_radar_csv == NULL) {
                fprintf(stderr, "[RADAR_DATA] Reopen failed for %s: %s\n", g_radar_csv_path,
                        strerror(errno));
            } else {
                printf("[系统] [RADAR_DATA] Rotated at %u MiB (keep=%d)\n",
                       TELEMETRY_LOG_MAX_BYTES / (1024U * 1024U), TELEMETRY_LOG_BACKUPS);
            }
        }
    }

    if (g_radar_state_path[0] == '\0')
        return;
    char tmp_path[PATH_MAX];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", g_radar_state_path) >= (int)sizeof(tmp_path))
        return;

    FILE *fp = open_file(tmp_path, "w", O_WRONLY | O_CREAT | O_TRUNC);
    if (fp == NULL)
        return;

    fprintf(fp,
            "{\"timestamp\":\"%s\",\"timestamp_ms\":%llu,"
            "\"radar_available\":%s,\"has_target\":%s,\"obj_count\":%d,"
            "\"dangerous_objId\":",
            timestamp, (unsigned long long)timestamp_ms, available ? "true" : "false",
            radar->has_target ? "true" : "false", radar->obj_count);
    if (radar->dangerous_index >= 0)
        fprintf(fp, "%d", radar->dangerous_obj_id);
    else
        fputs("null", fp);

    fputs(",\"distance_m\":", fp);
    if (radar->dangerous_index >= 0)
        fprintf(fp, "%.3f", radar->targets[radar->dangerous_index].distance);
    else
        fputs("null", fp);
    fputs(",\"velocity_mps\":", fp);
    if (radar->dangerous_index >= 0)
        fprintf(fp, "%.3f", radar->targets[radar->dangerous_index].velocity);
    else
        fputs("null", fp);
    fputs(",\"angle_deg\":", fp);
    if (radar->dangerous_index >= 0)
        fprintf(fp, "%.3f", radar->targets[radar->dangerous_index].angle);
    else
        fputs("null", fp);
    fputs(",\"filtered_angle_deg\":", fp);
    if (radar->dangerous_index >= 0)
        fprintf(fp, "%.3f", radar->targets[radar->dangerous_index].filtered_angle);
    else
        fputs("null", fp);
    fputs(",\"TTC_s\":", fp);
    if (radar->dangerous_index >= 0)
        json_write_float_or_null(fp, radar->targets[radar->dangerous_index].ttc);
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
                ? radar_direction_name(radar->targets[radar->dangerous_index].direction)
                : "UNKNOWN",
            radar->should_alert ? "true" : "false", fusion_alert ? "true" : "false",
            g_ttc_threshold, g_dist_threshold, g_angle_left_threshold, g_angle_right_threshold,
            g_angle_direction_sign);

    for (int i = 0; i < radar->obj_count; i++) {
        const radar_target_t *target = &radar->targets[i];
        if (i > 0)
            fputc(',', fp);
        fprintf(fp,
                "{\"objId\":%d,\"distance_m\":%.3f,"
                "\"velocity_mps\":%.3f,\"angle_deg\":%.3f,"
                "\"filtered_angle_deg\":%.3f,\"TTC_s\":",
                target->obj_id, target->distance, target->velocity, target->angle,
                target->filtered_angle);
        json_write_float_or_null(fp, target->ttc);
        fprintf(fp, ",\"direction\":\"%s\",\"is_dangerous\":%s}",
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

void radar_telemetry_publish_empty(void) {
    radar_result_t empty{};
    empty.dangerous_index = -1;
    empty.dangerous_obj_id = -1;
    radar_telemetry_publish(&empty, 0, false);
}

void radar_telemetry_close(void) {
    if (g_radar_csv != NULL) {
        fclose(g_radar_csv);
        g_radar_csv = NULL;
    }
}

static int dvr_storage_ok = 0;
static int g_storage_ready = 0;

static int dvr_storage_available(void) {
    struct stat mount_st;
    struct stat parent_st;
    return stat(g_dvr_mount_dir, &mount_st) == 0 && stat(g_dvr_mount_parent, &parent_st) == 0 &&
           S_ISDIR(mount_st.st_mode) && mount_st.st_dev != parent_st.st_dev;
}

static int path_requires_dvr_mount(const char *path) {
    size_t prefix_len = strlen(g_dvr_mount_dir);
    return path != NULL && strncmp(path, g_dvr_mount_dir, prefix_len) == 0 &&
           (path[prefix_len] == '\0' || path[prefix_len] == '/');
}

int finalize_dvr_storage_paths(void) {
    size_t mount_len = strlen(g_dvr_mount_dir);
    size_t base_len = strlen(g_dvr_base_dir);
    while (mount_len > 1 && g_dvr_mount_dir[mount_len - 1] == '/')
        g_dvr_mount_dir[--mount_len] = '\0';
    while (base_len > 1 && g_dvr_base_dir[base_len - 1] == '/')
        g_dvr_base_dir[--base_len] = '\0';

    if (!helmet::valid_storage_paths(g_dvr_mount_dir, g_dvr_base_dir)) {
        fprintf(stderr, "[DVR] dvr-dir must be an absolute child of dvr-mount-dir\n");
        return -1;
    }

    snprintf(g_dvr_mount_parent, sizeof(g_dvr_mount_parent), "%s", g_dvr_mount_dir);
    char *slash = strrchr(g_dvr_mount_parent, '/');
    if (slash == NULL)
        return -1;
    if (slash == g_dvr_mount_parent)
        slash[1] = '\0';
    else
        *slash = '\0';
    return 0;
}

/* 在主循环中重复调用；TF 晚到时只初始化一次，不阻塞风险处理。 */
void storage_try_initialize(void) {
    int storage_ready = dvr_storage_available();
    if (!dvr_storage_ok && storage_ready) {
        if (mkdir_recursive(g_dvr_base_dir) == 0) {
            dvr_storage_ok = 1;
            printf("[系统] [DVR] TF storage attached: %s\n", g_dvr_base_dir);
            startup_mark("dvr_storage_ready");
        }
    }

    int telemetry_ready = !path_requires_dvr_mount(g_radar_log_dir) || storage_ready;
    if (telemetry_ready)
        sensor_telemetry_init();
    if (telemetry_ready && g_radar_csv == NULL) {
        if (radar_telemetry_init() == 0)
            radar_telemetry_publish_empty();
    }

    if (!g_storage_ready && dvr_storage_ok && sensor_telemetry_available() && g_radar_csv != NULL) {
        g_storage_ready = 1;
        startup_mark("business_storage_initialized");
    }
}
