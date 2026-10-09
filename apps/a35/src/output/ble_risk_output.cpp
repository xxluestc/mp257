// Main 独占的方向灯输出：Fusion 风险方向 -> UART 文本命令 -> CH9140/WBA54。
// 期望状态与本地已发送状态分开维护，设备断开时保留期望值，重连后重新发送。
#include "ble_risk_output.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <limits.h>

#define BLE_UART_DEFAULT "/dev/ttySTM0"
#define BLE_RETRY_MS 2000ULL

static int g_ble_fd = -1;
static bool g_ble_enabled = true;
static bool g_ble_pending = true;
static BleRiskState g_ble_desired = BLE_RISK_CLEAR;
static BleRiskState g_ble_sent = BLE_RISK_CLEAR;
static char g_ble_uart[PATH_MAX] = BLE_UART_DEFAULT;
static unsigned long long g_ble_next_retry_ms;

static unsigned long long monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)ts.tv_nsec / 1000000ULL;
}

const char *ble_risk_state_name(BleRiskState state) {
    switch (state) {
    case BLE_RISK_LEFT:
        return "LEFT";
    case BLE_RISK_CENTER:
        return "CENTER";
    case BLE_RISK_RIGHT:
        return "RIGHT";
    default:
        return "CLEAR";
    }
}

static const char *ble_risk_command(BleRiskState state) {
    switch (state) {
    case BLE_RISK_LEFT:
        return "RISK LEFT\n";
    case BLE_RISK_CENTER:
        return "RISK CENTER\n";
    case BLE_RISK_RIGHT:
        return "RISK RIGHT\n";
    default:
        return "RISK CLEAR\n";
    }
}

static void ble_close(void) {
    if (g_ble_fd >= 0) {
        close(g_ble_fd);
        g_ble_fd = -1;
    }
}

static int ble_open(void) {
    struct termios tty;
    unsigned long long now = monotonic_ms();

    if (g_ble_fd >= 0)
        return 0;
    if (now < g_ble_next_retry_ms)
        return -1;

    g_ble_fd = open(g_ble_uart, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (g_ble_fd < 0) {
        fprintf(stderr, "[BLE-LED] Cannot open %s: %s; retrying\n", g_ble_uart, strerror(errno));
        g_ble_next_retry_ms = now + BLE_RETRY_MS;
        return -1;
    }

    if (tcgetattr(g_ble_fd, &tty) != 0) {
        fprintf(stderr, "[BLE-LED] tcgetattr(%s) failed: %s\n", g_ble_uart, strerror(errno));
        ble_close();
        g_ble_next_retry_ms = now + BLE_RETRY_MS;
        return -1;
    }

    cfmakeraw(&tty);
    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);
    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
    tty.c_cflag |= CS8;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;
    if (tcsetattr(g_ble_fd, TCSANOW, &tty) != 0) {
        fprintf(stderr, "[BLE-LED] tcsetattr(%s) failed: %s\n", g_ble_uart, strerror(errno));
        ble_close();
        g_ble_next_retry_ms = now + BLE_RETRY_MS;
        return -1;
    }

    tcflush(g_ble_fd, TCIOFLUSH);
    g_ble_pending = true;
    printf("[系统] [BLE-LED] CH9140 UART ready: %s @ 115200\n", g_ble_uart);
    return 0;
}

static void ble_drain_reply(void) {
    char reply[96];
    ssize_t total = 0;

    if (g_ble_fd < 0)
        return;

    while (total < (ssize_t)(sizeof(reply) - 1)) {
        ssize_t count = read(g_ble_fd, reply + total, sizeof(reply) - 1 - (size_t)total);
        if (count > 0) {
            total += count;
            continue;
        }
        if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "[BLE-LED] UART read failed: %s\n", strerror(errno));
            ble_close();
            g_ble_next_retry_ms = monotonic_ms() + BLE_RETRY_MS;
        }
        break;
    }

    if (total > 0) {
        reply[total] = '\0';
        printf("[BLE-LED] WBA reply: %s", reply);
        if (reply[total - 1] != '\n')
            printf("\n");
    }
}

void ble_risk_configure(const char *uart_device, bool enabled) {
    g_ble_enabled = enabled;
    if (uart_device != NULL && uart_device[0] != '\0')
        snprintf(g_ble_uart, sizeof(g_ble_uart), "%s", uart_device);
    g_ble_pending = true;
    if (!enabled)
        ble_close();
}

void ble_risk_update(BleRiskState state) {
    const char *command;
    size_t length;
    ssize_t written;

    if (!g_ble_enabled)
        return;

    if (state < BLE_RISK_CLEAR || state > BLE_RISK_RIGHT)
        state = BLE_RISK_CLEAR;
    if (state != g_ble_desired) {
        g_ble_desired = state;
        g_ble_pending = true;
    }

    if (ble_open() != 0)
        return;

    // 接收区只读取一份有界回复；状态未变化且已发送成功时，本轮不重复写命令。
    ble_drain_reply();
    if (!g_ble_pending)
        return;

    command = ble_risk_command(g_ble_desired);
    length = strlen(command);
    written = write(g_ble_fd, command, length);
    // 文本命令必须整行写出；短写时断开重连并保留 pending，避免认为状态已更新。
    if (written != (ssize_t)length) {
        if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            fprintf(stderr, "[BLE-LED] UART write failed: %s\n", strerror(errno));
        ble_close();
        g_ble_next_retry_ms = monotonic_ms() + BLE_RETRY_MS;
        return;
    }

    tcdrain(g_ble_fd);
    g_ble_sent = g_ble_desired;
    g_ble_pending = false;
    printf("[BLE-LED] Collision indication -> %s\n", ble_risk_state_name(g_ble_sent));
}

void ble_risk_shutdown(void) {
    if (g_ble_enabled && g_ble_fd >= 0) {
        static const char clear_command[] = "RISK CLEAR\n";
        ssize_t ignored = write(g_ble_fd, clear_command, sizeof(clear_command) - 1U);
        if (ignored > 0)
            (void)tcdrain(g_ble_fd);
    }
    ble_close();
}
