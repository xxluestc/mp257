/**
  ******************************************************************************
  * @file    main.c
  * @brief   HUD 主程序：OLED 导航显示 + 完整导航文字接收 + 骨传导播放接口 + IMU 转发
  ******************************************************************************
  */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#include "common.h"
#include "udp.h"
#include "oled.h"
#include "cJSON.h"

// ==================== 宏定义 ====================
#define NAV_UDP_PORT 8888              // 接收 APP 导航指令的端口
#define IMU_LOCAL_PORT 8890            // 接收脚本转发 IMU 数据的本地端口
#define APP_BROADCAST_PORT 8889        // 广播给 APP 的端口
#define BROADCAST_IP "192.168.152.255" // 广播地址
#define ALERT_COOLDOWN 60              // 冷却时间（秒）
#define NAV_TTS_TEXT_MAX 1024          // 完整导航播报文案最大字节数（UTF-8）
#define ENABLE_BONE_TTS_INTERFACE 1    // 1：调用骨传导播放接口；0：只打印完整导航文字

// ==================== 全局变量 ====================
static volatile int keep_running = 1;
static int oled_initialized = 0;
static time_t last_alert_ts = 0;

// ==================== 完整导航语音数据 ====================
// APP 发送示例：
// {"type":"navi_tts","tts_type":1,"seq":12,"text":"前方100米右转进入人民路"}
//
// 接收端只取 text 字段。
// text 是高德已经生成好的完整导航播报文字，可直接交给骨传导 TTS 播放，
// 不需要再拼接方向、距离或道路名称。
typedef struct {
    char text[NAV_TTS_TEXT_MAX];
} NaviTtsData;

// ==================== 信号处理 ====================
void sig_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        keep_running = 0;
    }
}

// ==================== 转向指令转文本 ====================
static const char *turn_to_text(int turn) {
    switch (turn) {
        case TURN_UNKNOWN: return "Unknown";
        case TURN_SELF_CAR: return "Self";
        case TURN_LEFT: return "Left";
        case TURN_RIGHT: return "Right";
        case TURN_SLIGHT_LEFT: return "SlightL";
        case TURN_SLIGHT_RIGHT: return "SlightR";
        case TURN_BACK_LEFT: return "BackL";
        case TURN_BACK_RIGHT: return "BackR";
        case TURN_UTURN_LEFT: return "U-turnL";
        case TURN_STRAIGHT: return "Straight";
        case TURN_VIA_POINT: return "Waypoint";
        case TURN_ROUNDABOUT: return "Roundabout";
        case TURN_EXIT_ROUNDABOUT: return "ExitRnd";
        case TURN_SERVICE: return "Service";
        case TURN_TOLL: return "Toll";
        case TURN_DESTINATION: return "Dest";
        case TURN_UTURN_RIGHT: return "U-turnR";
        default: return "?";
    }
}

// ==================== 解析导航 JSON ====================
int parse_navi_json(const char *json_str, NavData *nav) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) return -1;

    cJSON *type_obj = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type_obj) && strcmp(type_obj->valuestring, "navi") != 0) {
        cJSON_Delete(root);
        return -1;
    }

    cJSON *turn_obj = cJSON_GetObjectItem(root, "turn");
    cJSON *dist_obj = cJSON_GetObjectItem(root, "distance");

    if (!cJSON_IsNumber(turn_obj) || !cJSON_IsNumber(dist_obj)) {
        cJSON_Delete(root);
        return -1;
    }

    nav->turn = turn_obj->valueint;
    nav->distance = dist_obj->valueint;

    const char *text = turn_to_text(nav->turn);
    strncpy(nav->direction_text, text, sizeof(nav->direction_text) - 1);
    nav->direction_text[sizeof(nav->direction_text) - 1] = '\0';

    cJSON_Delete(root);
    return 0;
}

// ==================== OLED 安全显示 ====================
static void oled_show_nav_safe(NavData *nav, int has_signal) {
    if (oled_initialized) {
        oled_show_nav(nav, has_signal);
    }
}

// ==================== 解析完整导航播报 JSON ====================
/**
 * @brief 从 APP 的 navi_tts 数据中取出完整导航播报文字。
 *
 * APP 发送格式：
 * {
 *   "type":"navi_tts",
 *   "tts_type":1,
 *   "seq":12,
 *   "text":"前方100米右转进入人民路"
 * }
 *
 * 本函数只取 text 字段。text 已经是完整、可直接播放的导航文字。
 *
 * @return 0：解析成功；-1：不是 navi_tts 或 text 无效。
 */
static int parse_navi_tts_json(const char *json_str, NaviTtsData *tts_data) {
    if (json_str == NULL || tts_data == NULL) {
        return -1;
    }

    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        return -1;
    }

    cJSON *type_obj = cJSON_GetObjectItem(root, "type");
    cJSON *text_obj = cJSON_GetObjectItem(root, "text");

    if (!cJSON_IsString(type_obj) ||
        type_obj->valuestring == NULL ||
        strcmp(type_obj->valuestring, "navi_tts") != 0 ||
        !cJSON_IsString(text_obj) ||
        text_obj->valuestring == NULL ||
        text_obj->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return -1;
    }

    memset(tts_data, 0, sizeof(*tts_data));

    // text 为 UTF-8 完整导航播报文字，直接复制给骨传导接口使用。
    strncpy(tts_data->text, text_obj->valuestring, sizeof(tts_data->text) - 1);
    tts_data->text[sizeof(tts_data->text) - 1] = '\0';

    cJSON_Delete(root);
    return 0;
}

// ============================================================================
//                         骨传导语音播放接口
// ============================================================================
/**
 * @brief 播放一条完整导航语音。
 *
 * @param text 高德返回的完整导航播报文字，UTF-8 编码。
 *             例如："前方100米右转进入人民路"。
 *
 * 接口用法：
 * 1. text 已经是完整、可直接播放的文字信息；
 * 2. 不需要重新解析 JSON；
 * 3. 不需要根据方向和距离重新拼接文案；
 * 4. 队友只需在本函数内调用骨传导 TTS 播放函数。
 *
 * 接入示例：
 *     bone_tts_speak_utf8(text);
 *
 * 队友也可以在单独的 .c 文件中实现同名函数，覆盖这里的默认空实现。
 */
#if defined(__GNUC__)
__attribute__((weak))
#endif
void bone_conduction_speak(const char *text) {
    (void)text;

    // ==================== 骨传导接口实现位置 ====================
    // text 是完整、可直接播放的导航文字。
    // 在这里调用队友的骨传导 TTS 播放函数：
    // bone_tts_speak_utf8(text);
    // ============================================================
}

/**
 * @brief 打印完整导航文字，并调用骨传导播放接口。
 */
static void handle_navi_tts(const NaviTtsData *tts_data) {
    if (tts_data == NULL || tts_data->text[0] == '\0') {
        return;
    }

    // 只打印完整导航文字；不打印 OLED 使用的方向和距离简单指令。
    printf("[完整导航指令] %s\n", tts_data->text);
    fflush(stdout);

#if ENABLE_BONE_TTS_INTERFACE
    bone_conduction_speak(tts_data->text);
#endif
}

static void broadcast_imu_json(const char *json_str) {
    printf("[IMU] recv: %s\n", json_str);

    // 去除末尾换行符
    char clean_json[512];
    strncpy(clean_json, json_str, sizeof(clean_json) - 1);
    clean_json[sizeof(clean_json) - 1] = '\0';

    // 去除末尾的 \r \n
    int len = strlen(clean_json);
    while (len > 0 && (clean_json[len-1] == '\n' || clean_json[len-1] == '\r' || clean_json[len-1] == ' ')) {
        clean_json[len-1] = '\0';
        len--;
    }

    printf("[IMU] clean: %s\n", clean_json);

    cJSON *root = cJSON_Parse(clean_json);
    if (!root) {
        printf("[IMU] JSON parse fail\n");
        return;
    }

    cJSON *type_item = cJSON_GetObjectItem(root, "type");
    cJSON *message_item = cJSON_GetObjectItem(root, "message");

    if (!type_item) {
        printf("[IMU] missing type\n");
        cJSON_Delete(root);
        return;
    }

    const char *type = type_item->valuestring;
    const char *message = message_item ? message_item->valuestring : type;

    time_t now = time(NULL);
    if (now - last_alert_ts < ALERT_COOLDOWN) {
        printf("[IMU] cooldown, skip\n");
        cJSON_Delete(root);
        return;
    }
    last_alert_ts = now;

    char out[256];
    snprintf(out, sizeof(out),
             "{\"type\":\"%s\",\"message\":\"%s\",\"source\":\"IMU\"}",
             type, message);

    printf("[IMU] → APP: %s\n", out);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("[IMU] socket fail");
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

    ssize_t sent = sendto(sock, out, strlen(out), 0,
                          (struct sockaddr*)&dest, sizeof(dest));

    if (sent > 0) {
        printf("[IMU] ✅ UDP broadcast OK (%ld bytes)\n", (long)sent);
    } else {
        perror("[IMU] ❌ UDP broadcast fail");
    }

    close(sock);
    cJSON_Delete(root);
}

// ==================== 主程序 ====================
int main() {
    FILE *pidf = fopen("/tmp/hud.pid", "w");
    if (pidf) {
        fprintf(pidf, "%d", getpid());
        fclose(pidf);
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("[MAIN] HUD starting...\n");

    // ===== OLED 初始化 =====
    if (oled_init() != 0) {
        fprintf(stderr, "[MAIN] ⚠️ OLED init failed, display disabled\n");
        oled_initialized = 0;
    } else {
        oled_initialized = 1;
        printf("[MAIN] ✅ OLED initialized\n");
    }

    NavData nav = {0};
    oled_show_nav_safe(&nav, 0);

    // ===== UDP：接收导航指令 =====
    int nav_sock = udp_init(NAV_UDP_PORT);
    if (nav_sock < 0) {
        fprintf(stderr, "[UDP] udp_init failed, port=%d\n", NAV_UDP_PORT);
        if (oled_initialized) oled_close();
        return -1;
    }
    printf("[UDP] 导航监听端口: %d\n", NAV_UDP_PORT);

    // ===== UDP：接收脚本转发的 IMU 数据（JSON 格式） =====
    int imu_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (imu_sock < 0) {
        perror("[IMU] socket 创建失败");
        if (oled_initialized) oled_close();
        return -1;
    }

    int reuse = 1;
    setsockopt(imu_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in imu_addr;
    memset(&imu_addr, 0, sizeof(imu_addr));
    imu_addr.sin_family = AF_INET;
    imu_addr.sin_port = htons(IMU_LOCAL_PORT);
    imu_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(imu_sock, (struct sockaddr*)&imu_addr, sizeof(imu_addr)) < 0) {
        perror("[IMU] bind 失败");
        close(imu_sock);
        if (oled_initialized) oled_close();
        return -1;
    }
    printf("[IMU] 本地接收端口: %d (等待脚本转发 JSON 数据)\n", IMU_LOCAL_PORT);

    // ===== poll 监听 =====
    struct pollfd fds[2];
    fds[0].fd = nav_sock;
    fds[0].events = POLLIN;
    fds[1].fd = imu_sock;
    fds[1].events = POLLIN;

    char buffer[2048];  // 同时容纳 OLED 简单指令和完整 UTF-8 导航文案
    char imu_buffer[1024];
    time_t last_recv_time = 0;
    int has_signal = 0;

    while (keep_running) {
        int ret = poll(fds, 2, 100);

        if (ret < 0) {
            if (keep_running) perror("[POLL] poll error");
            continue;
        }

        // ===== 处理导航 UDP 数据（来自 APP） =====
        // 同一个 8888 端口接收两类 JSON：
        // 1. type=navi：原有方向+距离，仅更新 OLED，不打印简单指令；
        // 2. type=navi_tts：完整导航文字，打印并交给骨传导播放接口。
        if (fds[0].revents & POLLIN) {
            int n = udp_receive(nav_sock, buffer, sizeof(buffer) - 1, 0);
            if (n > 0) {
                buffer[n] = '\0';

                NaviTtsData tts_tmp;
                if (parse_navi_tts_json(buffer, &tts_tmp) == 0) {
                    // 完整导航文字 -> 打印 -> 骨传导播放接口。
                    // 注意：navi_tts 不刷新 OLED 信号时间，也不修改 OLED 内容。
                    handle_navi_tts(&tts_tmp);
                } else {
                    NavData nav_tmp;
                    if (parse_navi_json(buffer, &nav_tmp) == 0) {
                        // 原有链路保持不变：方向+距离只负责 OLED 显示。
                        memcpy(&nav, &nav_tmp, sizeof(NavData));
                        oled_show_nav_safe(&nav, 1);
                        last_recv_time = time(NULL);
                        has_signal = 1;

                        // 按需求：这里不再打印 turn、distance 或原始 navi JSON。
                    }
                }
            }
        }

        // ===== 处理 IMU 数据（来自脚本的 JSON） =====
        if (fds[1].revents & POLLIN) {
            int n = recvfrom(imu_sock, imu_buffer, sizeof(imu_buffer) - 1, 0,
                             NULL, NULL);
            if (n > 0) {
                imu_buffer[n] = '\0';
                // 检查是否为 JSON 格式
                if (strstr(imu_buffer, "{") != NULL) {
                    broadcast_imu_json(imu_buffer);
                } else {
                    printf("[IMU] 收到非 JSON 数据，忽略: %s\n", imu_buffer);
                }
            }
        }

        // ===== 导航超时处理 =====
        if (has_signal && time(NULL) - last_recv_time > 3) {
            printf("[OLED] 导航超时，显示 NO DATA\n");
            oled_show_nav_safe(&nav, 0);
            has_signal = 0;
        }
    }

    printf("[MAIN] exiting...\n");
    udp_close(nav_sock);
    close(imu_sock);
    if (oled_initialized) oled_close();
    return 0;
}