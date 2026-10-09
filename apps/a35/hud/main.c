/**
  ******************************************************************************
  * @file    main.c
  * @brief   HUD 事件转发：接收本机 IMU JSON 并转发手机
  ******************************************************************************
  */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#include "udp.h"
#include "cJSON.h"
#include "imu_message.h"

// ==================== 宏定义 ====================
#define IMU_LOCAL_PORT 8890            // 接收脚本转发 IMU 数据的本地端口
#define APP_BROADCAST_PORT 8889        // 广播给 APP 的端口
#define BROADCAST_IP "192.168.152.255" // 广播地址
#define ALERT_COOLDOWN 60              // 冷却时间（秒）
#define DELIVERY_LOG_DIR "/usr/local/helmet/radar_experiments"
#define DELIVERY_LOG_PATH DELIVERY_LOG_DIR "/imu_delivery.csv"
#define DELIVERY_LOG_MAX_BYTES (10U * 1024U * 1024U)
#define DELIVERY_LOG_BACKUPS 4
#define DELIVERY_LOG_SIZE_CHECK_WRITES 256U

// ==================== 全局变量 ====================
static volatile sig_atomic_t keep_running = 1;
/* 每类事件独立冷却，避免频繁 road_bump 吞掉真正的 fall_down。 */
static time_t last_alert_ts[3] = {0, 0, 0};
static FILE *delivery_log = NULL;
static unsigned int delivery_log_write_count = 0;

static const char DELIVERY_LOG_HEADER[] =
    "timestamp_ms,event_id,m33_type,app_type,seq,reason,stage,status,bytes\n";

static long long wall_clock_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000LL;
}

static int delivery_log_rotate(void) {
    char source[1024];
    char destination[1024];
    for (int index = DELIVERY_LOG_BACKUPS; index >= 1; index--) {
        if (index == 1)
            snprintf(source, sizeof(source), "%s", DELIVERY_LOG_PATH);
        else
            snprintf(source, sizeof(source), "%s.%d", DELIVERY_LOG_PATH, index - 1);
        snprintf(destination, sizeof(destination), "%s.%d", DELIVERY_LOG_PATH, index);
        if (index == DELIVERY_LOG_BACKUPS)
            unlink(destination);
        if (rename(source, destination) != 0 && errno != ENOENT) {
            fprintf(stderr, "[IMU] log rotate failed: %s\n", strerror(errno));
            return -1;
        }
    }
    return 0;
}

static void delivery_log_open(void) {
    mkdir(DELIVERY_LOG_DIR, 0775);
    struct stat st;
    if (stat(DELIVERY_LOG_PATH, &st) == 0 && st.st_size >= (off_t)DELIVERY_LOG_MAX_BYTES)
        delivery_log_rotate();

    int needs_header = stat(DELIVERY_LOG_PATH, &st) != 0 || st.st_size == 0;
    delivery_log = fopen(DELIVERY_LOG_PATH, "a");
    if (delivery_log == NULL) {
        fprintf(stderr, "[IMU] delivery log open failed: %s\n", strerror(errno));
        return;
    }
    setvbuf(delivery_log, NULL, _IOLBF, BUFSIZ);
    if (needs_header) {
        /* UTF-8 BOM，兼容 Excel/WPS 直接打开 CSV。 */
        fputs("\xEF\xBB\xBF", delivery_log);
        fputs(DELIVERY_LOG_HEADER, delivery_log);
    }
    delivery_log_write_count = 0;
    printf("[IMU] delivery log: %s (10 MiB x current+4)\n", DELIVERY_LOG_PATH);
}

static void delivery_log_write(const char *event_id, const char *m33_type, const char *app_type,
                               int seq, const char *reason, const char *stage, const char *status,
                               long bytes) {
    if (delivery_log == NULL)
        return;
    fprintf(delivery_log, "%lld,%s,%s,%s,%d,%s,%s,%s,%ld\n", wall_clock_ms(),
            event_id ? event_id : "", m33_type ? m33_type : "", app_type ? app_type : "", seq,
            reason ? reason : "", stage ? stage : "", status ? status : "", bytes);

    delivery_log_write_count++;
    if (delivery_log_write_count % DELIVERY_LOG_SIZE_CHECK_WRITES == 0) {
        struct stat st;
        if (fflush(delivery_log) == 0 && fstat(fileno(delivery_log), &st) == 0 &&
            st.st_size >= (off_t)DELIVERY_LOG_MAX_BYTES) {
            fclose(delivery_log);
            delivery_log = NULL;
            delivery_log_rotate();
            delivery_log_open();
        }
    }
}

static int alert_slot(const char *type) {
    if (type != NULL && strcmp(type, "fall_down") == 0)
        return 0;
    if (type != NULL && strcmp(type, "emergency_brake") == 0)
        return 1;
    return 2;
}

static const char *json_string(cJSON *root, const char *name, const char *fallback) {
    cJSON *item = cJSON_GetObjectItem(root, name);
    return cJSON_IsString(item) && item->valuestring != NULL ? item->valuestring : fallback;
}

static int json_int(cJSON *root, const char *name) {
    cJSON *item = cJSON_GetObjectItem(root, name);
    return cJSON_IsNumber(item) ? item->valueint : 0;
}

static time_t monotonic_seconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return now.tv_sec;
}

static void sig_handler(int sig) {
    (void)sig;
    keep_running = 0;
}

static void broadcast_imu_json(const char *json_str) {
    printf("[IMU] recv: %s\n", json_str);

    // 去除末尾换行符
    char clean_json[2048];
    strncpy(clean_json, json_str, sizeof(clean_json) - 1);
    clean_json[sizeof(clean_json) - 1] = '\0';

    // 去除末尾的 \r \n
    int len = strlen(clean_json);
    while (len > 0 && (clean_json[len - 1] == '\n' || clean_json[len - 1] == '\r' ||
                       clean_json[len - 1] == ' ')) {
        clean_json[len - 1] = '\0';
        len--;
    }

    printf("[IMU] clean: %s\n", clean_json);

    cJSON *root = cJSON_ParseWithOpts(clean_json, NULL, 1);
    if (!root) {
        printf("[IMU] JSON parse fail\n");
        return;
    }

    cJSON *type_item = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type_item) || type_item->valuestring == NULL) {
        printf("[IMU] missing type\n");
        cJSON_Delete(root);
        return;
    }

    const char *type = type_item->valuestring;
    const char *event_id = json_string(root, "event_id", "unknown");
    const char *m33_type = json_string(root, "m33_type", "unknown");
    const char *reason = json_string(root, "reason", "unknown");
    int seq = json_int(root, "seq");
    delivery_log_write(event_id, m33_type, type, seq, reason, "hud_received", "ok", len);

    time_t now = monotonic_seconds();
    int slot = alert_slot(type);
    if (last_alert_ts[slot] && now - last_alert_ts[slot] < ALERT_COOLDOWN) {
        printf("[IMU] same-type cooldown, skip type=%s\n", type);
        delivery_log_write(event_id, m33_type, type, seq, reason, "app_broadcast",
                           "suppressed_cooldown", 0);
        cJSON_Delete(root);
        return;
    }

    // Update the parsed object and serialize it through cJSON: incoming text
    // remains escaped and GPS/other fields survive forwarding unchanged.
    char *out = imu_message_to_json(root, (double)wall_clock_ms());
    if (!out) {
        cJSON_Delete(root);
        return;
    }

    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock < 0) {
        perror("[IMU] socket fail");
        delivery_log_write(event_id, m33_type, type, seq, reason, "app_broadcast", "socket_failed",
                           -1);
        cJSON_free(out);
        cJSON_Delete(root);
        return;
    }

    int on = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(APP_BROADCAST_PORT);
    inet_aton(BROADCAST_IP, &dest.sin_addr);

    ssize_t sent = sendto(sock, out, strlen(out), 0, (struct sockaddr *)&dest, sizeof(dest));

    if (sent > 0) {
        printf("[IMU] ✅ UDP broadcast OK (%ld bytes)\n", (long)sent);
        last_alert_ts[slot] = now;
        delivery_log_write(event_id, m33_type, type, seq, reason, "app_broadcast", "sent",
                           (long)sent);
    } else {
        perror("[IMU] ❌ UDP broadcast fail");
        delivery_log_write(event_id, m33_type, type, seq, reason, "app_broadcast", "send_failed",
                           (long)sent);
    }

    close(sock);
    cJSON_free(out);
    cJSON_Delete(root);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, BUFSIZ);
    setvbuf(stderr, NULL, _IOLBF, BUFSIZ);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    int imu_sock = udp_init_local(IMU_LOCAL_PORT);
    if (imu_sock < 0)
        return 1;
    FILE *pidfile = fopen("/tmp/hud.pid", "w");
    if (pidfile) {
        fprintf(pidfile, "%d\n", (int)getpid());
        fclose(pidfile);
    }
    delivery_log_open();
    printf("[IMU] Local relay ready on 127.0.0.1:%d\n", IMU_LOCAL_PORT);
    char message[2048];
    while (keep_running) {
        int count = udp_receive(imu_sock, message, sizeof(message), 100);
        if (count > 0)
            broadcast_imu_json(message);
        else if (count == -1)
            break;
    }
    udp_close(imu_sock);
    if (delivery_log)
        fclose(delivery_log);
    unlink("/tmp/hud.pid");
    return keep_running ? 1 : 0;
}
