#include "app/services.hpp"
#include "runtime/frame_pipeline.hpp"
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <cerrno>
#include <sys/ioctl.h>
#include <time.h>
#include <linux/gpio.h>

double boot_time_seconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0)
        return -1.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

void startup_mark(const char *milestone) {
    printf("[启动] [boot=%.3fs] %s\n", boot_time_seconds(), milestone);
}

static int g_led_fd = -1;

int gpio_init(void) {
    int chip_fd = open(GPIO_CHIP_DEV, O_RDONLY | O_CLOEXEC);
    if (chip_fd < 0) {
        fprintf(stderr, "[LED] Cannot open %s: %s\n", GPIO_CHIP_DEV, strerror(errno));
        return -1;
    }
    struct gpiohandle_request req;
    memset(&req, 0, sizeof(req));
    req.lineoffsets[0] = GPIO_LED_LINE;
    req.flags = GPIOHANDLE_REQUEST_OUTPUT;
    req.default_values[0] = 0;
    req.lines = 1;
    strncpy(req.consumer_label, "radar_fusion", sizeof(req.consumer_label) - 1);
    if (ioctl(chip_fd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) {
        fprintf(stderr, "[LED] GPIO line request failed: %s\n", strerror(errno));
        close(chip_fd);
        return -1;
    }
    close(chip_fd);
    g_led_fd = req.fd;
    fcntl(g_led_fd, F_SETFD, FD_CLOEXEC);
    printf("[LED] GPIO PD11 ready\n");
    return 0;
}

/**
 * @brief 设置 GPIO LED 电平
 * @param val 0 熄灭，非 0 点亮
 */
static void gpio_set(int val) {
    if (g_led_fd < 0)
        return;
    struct gpiohandle_data data;
    data.values[0] = (uint8_t)(val ? 1 : 0);
    ioctl(g_led_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data);
}

/**
 * @brief 释放 GPIO LED 资源
 *
 * 熄灭 LED 并关闭 GPIO 线句柄。
 */
void gpio_deinit(void) {
    if (g_led_fd >= 0) {
        gpio_set(0);
        close(g_led_fd);
        g_led_fd = -1;
    }
}

void *led_thread(void *arg) {
    (void)arg;
    while (g_running) {
        if (g_led_alert) {
            gpio_set(1);
            usleep(LED_BLINK_ON_MS * 1000);
            gpio_set(0);
            usleep(LED_BLINK_OFF_MS * 1000);
        } else {
            gpio_set(0);
            usleep(100000);
        }
    }
    gpio_set(0);
    return NULL;
}

/* ======================== 设备冲突清理 ======================== */

/**
 * @brief 强制结束占用指定设备的进程
 * @param device 设备节点路径
 *
 * 使用 fuser 终止持有该设备的进程，避免摄像头/串口被占用导致打开失败。
 */
int set_uart(int fd, int baudrate) {
    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        perror("tcgetattr");
        return -1;
    }
    speed_t speed;
    switch (baudrate) {
    case 9600:
        speed = B9600;
        break;
    case 115200:
        speed = B115200;
        break;
    case 921600:
        speed = B921600;
        break;
    default:
        fprintf(stderr, "Unsupported baudrate: %d\n", baudrate);
        return -1;
    }
    cfsetospeed(&tty, speed);
    cfsetispeed(&tty, speed);
    tty.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
    tty.c_cflag |= CS8 | CREAD | CLOCAL;
    tty.c_iflag &=
        ~(IXON | IXOFF | IXANY | IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
    tty.c_oflag &= ~OPOST & ~ONLCR;
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;
    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        perror("tcsetattr");
        return -1;
    }
    tcflush(fd, TCIOFLUSH);
    return 0;
}

/**
 * @brief 计算 16 位累加和校验
 * @param data 数据指针
 * @param len  数据长度
 * @return 16 位校验和
 */
static uint16_t calc_sum16(const uint8_t *data, int len) {
    uint32_t sum = 0;
    for (int i = 0; i < len; i++)
        sum += data[i];
    return (uint16_t)(sum & 0xFFFF);
}

/**
 * @brief 计算 8 位累加和校验
 * @param data 数据指针
 * @param len  数据长度
 * @return 8 位校验和
 */
uint8_t calc_sum8(const uint8_t *data, int len) {
    uint32_t sum = 0;
    for (int i = 0; i < len; i++)
        sum += data[i];
    return (uint8_t)(sum & 0xFF);
}

/**
 * @brief 向雷达模块发送命令帧
 * @param fd        串口文件描述符
 * @param group     命令组号
 * @param cmd       命令号
 * @param params    命令参数缓冲区
 * @param param_len 参数长度
 * @return 0 成功，-1 失败
 *
 * 帧格式：HEAD_CMD (1B) | group+cmd (1B) | param_len (1B) | params (nB) | sum16 (2B)
 */
int send_cmd(int fd, uint8_t group, uint8_t cmd, const uint8_t *params, int param_len) {
    uint8_t frame[64];
    if (param_len < 0 || param_len > (int)sizeof(frame) - 5 || (param_len && !params)) {
        errno = EINVAL;
        return -1;
    }
    int idx = 0;
    frame[idx++] = HEAD_CMD;
    frame[idx++] = (group << 5) | (cmd & 0x1F);
    frame[idx++] = (uint8_t)param_len;
    if (params && param_len > 0) {
        memcpy(&frame[idx], params, param_len);
        idx += param_len;
    }
    uint16_t csum = calc_sum16(frame, idx);
    frame[idx++] = (uint8_t)(csum & 0xFF);
    frame[idx++] = (uint8_t)((csum >> 8) & 0xFF);
    int offset = 0;
    uint64_t deadline = helmet::monotonic_us() + 200000;
    while (offset < idx && g_running && helmet::monotonic_us() < deadline) {
        ssize_t written = write(fd, frame + offset, idx - offset);
        if (written > 0)
            offset += (int)written;
        else if (written < 0 && errno != EAGAIN && errno != EINTR)
            return -1;
        else
            usleep(1000);
    }
    return offset == idx ? 0 : -1;
}

/**
 * @brief 清空串口接收缓冲区的残余数据
 * @param fd 串口文件描述符
 */
void flush_rx(int fd) {
    uint8_t tmp[256];
    int n;
    while ((n = read(fd, tmp, sizeof(tmp))) > 0)
        ;
}
