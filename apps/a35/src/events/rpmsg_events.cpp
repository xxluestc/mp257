// M33 事件入口：RPMsg 文本 -> IMU/V2X 解析 -> 音频任务、摔倒邮箱及 HUD JSON 转发。
// 本线程不持有摄像头或录像文件，录像触发由 Main 转交给 DVR 队列。
#include "app/services.hpp"
#include "runtime/frame_pipeline.hpp"
#include "runtime/linux_resources.hpp"
#include "events/message_fields.hpp"
#include "nav_tts.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <cerrno>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

void play_recording_complete_sound(void) {
    const char *file = AUDIO_RECORDING_COMPLETE;
    if (access(file, F_OK) != 0) {
        printf("[AUDIO] Recording complete sound not found: %s\n", file);
        return;
    }
    if (nav_tts_play_file(file) != 0) {
        fprintf(stderr, "[AUDIO] Failed to play recording complete sound\n");
    } else {
        printf("[保存] [AUDIO] Playing recording complete notification\n");
    }
}

void play_alert_sound(const char *type) {
    const char *file = NULL;
    if (strcmp(type, "fall") == 0)
        file = AUDIO_FALL;
    else if (strcmp(type, "collision") == 0)
        file = AUDIO_COLLISION;
    else
        return;

    if (access(file, F_OK) != 0) {
        printf("[AUDIO] Sound file not found: %s\n", file);
        return;
    }

    if (nav_tts_play_file(file) != 0) {
        fprintf(stderr, "[AUDIO] Failed to play %s\n", file);
    } else {
        printf("[告警] [AUDIO] Playing %s alert\n", type);
    }
}

static void play_v2x_alert(const char *direction) {
    const char *file = NULL;
    if (strcmp(direction, "nearby") == 0)
        file = AUDIO_V2X_NEARBY;
    else if (strcmp(direction, "left_front") == 0)
        file = AUDIO_V2X_LEFT_FRONT;
    else if (strcmp(direction, "right_front") == 0)
        file = AUDIO_V2X_RIGHT_FRONT;
    else if (strcmp(direction, "left") == 0)
        file = AUDIO_V2X_LEFT;
    else if (strcmp(direction, "right") == 0)
        file = AUDIO_V2X_RIGHT;
    else
        file = AUDIO_V2X_NEARBY;

    if (access(file, F_OK) != 0) {
        printf("[AUDIO] V2X sound file not found: %s\n", file);
        return;
    }

    uint64_t now_us = helmet::monotonic_us();
    uint64_t previous = g_last_v2x_audio_us.load();
    if (previous != 0 && now_us - previous < V2X_AUDIO_COOLDOWN_US) {
        printf("[V2X] Audio cooldown, skip playing (%s)\n", direction);
        return;
    }
    if (!g_last_v2x_audio_us.compare_exchange_strong(previous, now_us))
        return;

    if (nav_tts_play_file(file) != 0) {
        fprintf(stderr, "[AUDIO] Failed to play v2x %s alert\n", direction);
    } else {
        printf("[告警] [AUDIO] Playing V2X alert: %s\n", direction);
    }
}

static void handle_v2x_alert(const char *direction) {
    if (!direction)
        direction = "nearby";
    printf("[告警] [V2X] V2X_ALERT direction=%s\n", direction);
    g_v2x_alert = 1;
    /* V2X 只走音频提示，不驱动 LED；LED 留给雷达碰撞/IMU 摔倒 */
    play_v2x_alert(direction);
}

/* ======================== IMU 摔倒触发处理 ======================== */
static void handle_fall_trigger(uint64_t trigger_time_us) {
    if (g_imu_fall_alert.exchange(1))
        return; /* 已触发, 忽略重复 */
    g_imu_fall_alert = 1;
    g_imu_fall_time_us = trigger_time_us;

    printf("[告警] [IMU] FALL DETECTED! Triggering emergency save\n");
    play_alert_sound("fall");
    /* RPMsg/测试线程只投递事件；DVR 状态统一由 DVR 线程操作，避免与摄像头
     * 写帧、雷达碰撞触发并发打开或关闭同一个缓冲文件。 */
    g_pending_fall_dvr_us.store(trigger_time_us, std::memory_order_release);
}

/* ======================== IMU 异常事件 UDP 转发到 HUD/App ======================== */
/**
 * @brief 发送 JSON 到 HUD 输入端口（HUD 再转发到手机 App）
 * @param json 要发送的 JSON 字符串
 *
 * 与 v2x_alert_link.sh 行为一致：UDP 127.0.0.1:8890 -> HUD -> App
 */
static int udp_send_to_hud(const char *json) {
    if (json == NULL || json[0] == '\0')
        return -1;

    helmet::UniqueFd descriptor(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
    int sock = descriptor.get();
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
        return -1;
    }

    ssize_t sent = sendto(sock, json, strlen(json), 0, (struct sockaddr *)&addr, sizeof(addr));
    if (sent < 0) {
        fprintf(stderr, "[IMU_FWD] sendto failed: %s\n", strerror(errno));
    } else {
        printf("[系统] [IMU_FWD] Forwarded %zd bytes to %s:%d\n", sent, HUD_INPUT_IP,
               HUD_INPUT_PORT);
    }
    return sent >= 0 ? (int)sent : -1;
}

/**
 * @brief 从 IMU_ALERT 行中提取 key=value 形式的整数值
 */
static int parse_kv_int(const char *line, const char *key) {
    if (line == NULL || key == NULL)
        return 0;
    return helmet::message_integer(line, key);
}

/**
 * @brief 解析并转发 IMU 异常事件到 HUD
 * @param line M33 输出的 IMU_ALERT 行
 *
 * 支持 type=fall / hard_brake / road_bump，与 v2x_alert_link.sh 映射一致。
 * 同时保留原有的摔倒触发逻辑（音频 + DVR）。
 */
static void forward_imu_alert(const char *line) {
    if (line == NULL || strstr(line, "IMU_ALERT") == NULL)
        return;

    // 按完整 key=value 字段匹配，避免把 falling 或其他字段中的 fall 误识别为摔倒。
    const auto type = helmet::message_field(line, "type");

    const char *app_type = NULL;
    const char *message = NULL;
    char m33_type[32] = {0};
    if (type == "fall") {
        app_type = "fall_down";
        message = "多用户在此摔倒，请减速慢行";
        snprintf(m33_type, sizeof(m33_type), "fall");
    } else if (type == "hard_brake") {
        app_type = "emergency_brake";
        message = "多用户急刹，请注意减速";
        snprintf(m33_type, sizeof(m33_type), "hard_brake");
    } else if (type == "road_bump") {
        app_type = "road_hazard";
        message = "前方路面颠簸，请注意避让";
        snprintf(m33_type, sizeof(m33_type), "road_bump");
    } else {
        printf("[IMU_FWD] Unknown IMU type, skip forwarding\n");
        return;
    }

    int seq = parse_kv_int(line, "seq");
    int gps_valid = parse_kv_int(line, "gps_valid");
    int lat_1e7 = parse_kv_int(line, "lat_1e7");
    int lon_1e7 = parse_kv_int(line, "lon_1e7");
    int speed_cms = parse_kv_int(line, "speed_cms");
    int heading_cdeg = parse_kv_int(line, "heading_cdeg");
    int tick = parse_kv_int(line, "tick");
    int acc_norm = parse_kv_int(line, "acc_norm");
    int horiz_acc = parse_kv_int(line, "horiz_acc");
    int z_delta = parse_kv_int(line, "z_delta");
    int brake_y_delta = parse_kv_int(line, "brake_y_delta");
    int ax = parse_kv_int(line, "ax");
    int ay = parse_kv_int(line, "ay");
    int az = parse_kv_int(line, "az");
    int gx = parse_kv_int(line, "gx");
    int gy = parse_kv_int(line, "gy");
    int gz = parse_kv_int(line, "gz");
    int roll = parse_kv_int(line, "roll");
    int pitch = parse_kv_int(line, "pitch");
    int yaw = parse_kv_int(line, "yaw");
    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);
    uint64_t timestamp_ms = (uint64_t)tv_now.tv_sec * 1000ULL + (uint64_t)tv_now.tv_usec / 1000ULL;

    const auto reason = helmet::message_field(line, "reason");
    char reason_buf[64] = "unknown";
    if (!reason.empty() && reason.size() < sizeof(reason_buf)) {
        memcpy(reason_buf, reason.data(), reason.size());
        reason_buf[reason.size()] = '\0';
    }

    char event_id[96];
    snprintf(event_id, sizeof(event_id), "m33-%d-%llu", seq, (unsigned long long)timestamp_ms);
    char details[512];
    snprintf(details, sizeof(details),
             "tick=%d acc_norm=%d horiz_acc=%d z_delta=%d brake_y_delta=%d "
             "ax=%d ay=%d az=%d gx=%d gy=%d gz=%d roll=%d pitch=%d yaw=%d",
             tick, acc_norm, horiz_acc, z_delta, brake_y_delta, ax, ay, az, gx, gy, gz, roll, pitch,
             yaw);
    sensor_event_log("imu_m33", m33_type, "received", event_id, NULL, -1.0f, -1, seq, reason_buf,
                     details);

    char json[1536];
    // reason 来自外部文本，必须转义引号、反斜杠和控制字符后才能嵌入 JSON。
    const auto escaped_reason = helmet::escape_json(reason_buf);
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
             app_type, message, m33_type, escaped_reason.c_str(), seq, event_id,
             (unsigned long long)timestamp_ms, strcmp(m33_type, "fall") == 0 ? "true" : "false",
             tick, acc_norm, horiz_acc, z_delta, brake_y_delta, ax, ay, az, gx, gy, gz, roll, pitch,
             yaw, gps_valid, lat_1e7, lon_1e7, speed_cms, heading_cdeg);

    printf("[系统] [IMU_FWD] Forwarding IMU event: type=%s\n", app_type);
    int bytes = udp_send_to_hud(json);
    char send_details[96];
    snprintf(send_details, sizeof(send_details), "udp_bytes=%d hud_port=%d", bytes, HUD_INPUT_PORT);
    sensor_event_log("a35_hud", app_type, bytes >= 0 ? "sent" : "failed", event_id, NULL, -1.0f, -1,
                     seq, reason_buf, send_details);
}

/* ======================== 测试模式: 模拟 IMU 摔倒触发 ======================== */

void *test_fall_thread(void *arg) {
    (void)arg;
    if (g_test_fall_delay_sec <= 0)
        return NULL;
    printf("[TEST] Simulating IMU fall after %d seconds...\n", g_test_fall_delay_sec);
    for (int i = 0; i < g_test_fall_delay_sec && g_running; i++)
        sleep(1);
    if (!g_running)
        return NULL;

    for (int event_index = 0; event_index < g_test_fall_count && g_running; event_index++) {
        if (event_index > 0) {
            printf("[TEST] Waiting %d seconds before repeated FALL %d/%d\n",
                   g_test_fall_interval_sec, event_index + 1, g_test_fall_count);
            for (int i = 0; i < g_test_fall_interval_sec && g_running; i++)
                sleep(1);
        }
        if (!g_running)
            break;

        uint64_t ts_us = helmet::monotonic_us();
        printf("[TEST] Injecting simulated FALL event %d/%d\n", event_index + 1, g_test_fall_count);
        handle_fall_trigger(ts_us);

        /* 模拟摔倒事件也触发 UDP 转发到 HUD/App，用于测试 */
        char simulated_line[256];
        snprintf(simulated_line, sizeof(simulated_line),
                 "IMU_ALERT type=fall reason=simulated seq=%d gps_valid=0 lat_1e7=0 lon_1e7=0",
                 event_index);
        forward_imu_alert(simulated_line);
    }

    return NULL;
}

/* ======================== 测试模式: 模拟 V2X 告警 ======================== */

void *test_v2x_thread(void *arg) {
    (void)arg;
    if (g_test_v2x_delay_sec <= 0)
        return NULL;
    printf("[TEST] Simulating V2X alert (%s) after %d seconds...\n", g_test_v2x_direction,
           g_test_v2x_delay_sec);
    for (int i = 0; i < g_test_v2x_delay_sec && g_running; i++)
        sleep(1);
    if (!g_running)
        return NULL;

    printf("[TEST] Injecting simulated V2X event\n");
    handle_v2x_alert(g_test_v2x_direction);
    return NULL;
}

/* ======================== RPMsg 接收线程 (M33 alerts) ======================== */
static void rpmsg_receive_once() {
    helmet::UniqueFd descriptor(open(RPMSG_DEVICE, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC));
    int fd = descriptor.get();
    if (fd < 0) {
        fprintf(stderr, "[IMU] Cannot open %s: %s\n", RPMSG_DEVICE, strerror(errno));
        return;
    }
    startup_mark("rpmsg_device_opened");

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    cfsetospeed(&tty, RPMSG_BAUD);
    cfsetispeed(&tty, RPMSG_BAUD);
    tty.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
    tty.c_cflag |= CS8 | CREAD | CLOCAL;
    tty.c_iflag &=
        ~(IXON | IXOFF | IXANY | IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
    tty.c_oflag &= ~OPOST & ~ONLCR;
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;
    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        fprintf(stderr, "[IMU] tcsetattr failed: %s\n", strerror(errno));
        return;
    }
    tcflush(fd, TCIOFLUSH);

    /*
     * M33 收到 ready 后才开始发送告警。保留原有1秒端点稳定时间，并等待
     * ALSA播放节点，避免业务前移后在音频尚未出现的窗口内收到摔倒/V2V事件、
     * 导致告警语音丢失。等待有界；音频异常不能永久阻塞M33链路。
     */
    sleep(1);
    int audio_ready = 0;
    for (int i = 0; i < 50 && g_running; i++) {
        if (access("/dev/snd/pcmC0D0p", F_OK) == 0) {
            audio_ready = 1;
            startup_mark("audio_output_ready");
            break;
        }
        usleep(100000);
    }
    if (!audio_ready)
        fprintf(stderr, "[IMU] Audio output not ready after 5s; "
                        "sending RPMsg ready with degraded audio\n");
    if (!g_running)
        return;
    if (write(fd, RPMSG_READY_MSG, strlen(RPMSG_READY_MSG)) != (ssize_t)strlen(RPMSG_READY_MSG)) {
        fprintf(stderr, "[IMU] Failed to send ready message: %s\n", strerror(errno));
        return;
    } else {
        tcdrain(fd);
        printf("[系统] [IMU] RPMsg ready message sent to M33\n");
        startup_mark("rpmsg_ready_sent");
    }

    // 只处理完整行。超长/含 NUL 行整体丢弃，不能把截断消息当作新告警。
    helmet::MessageLine<512> lines;
    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = {0, 50000}; /* 50ms timeout */
        int ret = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0 && errno != EINTR)
            break;

        if (ret > 0 && FD_ISSET(fd, &rfds)) {
            char c;
            ssize_t n = read(fd, &c, 1);
            if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN))
                break;
            if (n > 0) {
                if (const char *line = lines.append(c)) {

                    /* 解析 IMU 异常告警: IMU_ALERT ... type=... */
                    if (strstr(line, "IMU_ALERT") != NULL) {
                        uint64_t ts_us = helmet::monotonic_us();

                        /* 摔倒事件额外触发音频 + DVR 保存 */
                        if (helmet::message_field(line, "type") == "fall") {
                            handle_fall_trigger(ts_us);
                        }

                        /* 所有 IMU 异常统一通过 UDP 转发到 HUD/App */
                        forward_imu_alert(line);
                    }
                    /* V2X 告警: 解析 direction 并播放定向语音 */
                    else if (strstr(line, "V2X_ALERT") != NULL) {
                        char direction[32] = "nearby";
                        const auto field = helmet::message_field(line, "direction");
                        if (!field.empty() && field.size() < sizeof(direction)) {
                            memcpy(direction, field.data(), field.size());
                            direction[field.size()] = '\0';
                        }
                        handle_v2x_alert(direction);
                    }
                }
            }
        }
    }

    printf("[系统] [IMU] RPMsg endpoint closed\n");
}

void *rpmsg_thread(void *arg) {
    (void)arg;
    try {
        while (g_running) {
            // 一次连接结束即回收 fd；重连间隔拆成短等待，停机不必等满两秒。
            rpmsg_receive_once();
            for (int attempt = 0; attempt < 20 && g_running; ++attempt)
                usleep(100000);
        }
    } catch (const std::exception &error) {
        fprintf(stderr, "[IMU] Receiver failed: %s\n", error.what());
    }
    return NULL;
}
