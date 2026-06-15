/*
 * MS60-1211S80M-BSD (AT6010) 雷达初始化 + 持续接收 + PD11 LED TTC告警
 * 适配开发板：STM32 USART1 (PG14/PG15 AF6) -> /dev/ttySTM1
 *
 * ============================================================================
 *  如何修改参数并重新编译（在虚拟机中操作）:
 * ============================================================================
 *  1. 编辑本文件，修改下方 "可调参数" 区域的宏定义
 *  2. 编译:
 *       export PATH=/home/alientek/Phytium_syscode/GCC编译器/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/bin:$PATH
 *       aarch64-none-linux-gnu-gcc -Wall -O2 -o radar_init radar_init.c -lpthread
 *  3. 传到开发板:
 *       scp radar_init root@192.168.88.10:/xxl/radar/
 *  4. 在开发板上运行:
 *       ssh root@192.168.88.10
 *       cd /xxl/radar && pkill radar_init; ./radar_init
 *
 * ============================================================================
 *  室外 BSD 调试建议:
 * ============================================================================
 *  - TTC_THRESHOLD : 车辆盲区监测建议 3~5秒，人员靠近测试建议 5~10秒
 *  - DIST_THRESHOLD: 盲区边界距离，建议 2~5米
 *  - LED_BLINK_*   : 闪烁频率，室外建议 100~200ms
 *  - 感应等级(0x02): 通过 send_radar_cmd.sh 脚本动态调节，不用重新编译
 *                   室外误触发多 -> 调高(5~10)，漏检 -> 调低(0~2)
 *  - 检测距离(0xD2): 通过脚本动态调节，建议 10~30m
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/time.h>
#include <pthread.h>

#define UART_DEVICE     "/dev/ttySTM1"
#define BAUDRATE        921600

/* ============================================================================
 *  可调参数 - 修改后需重新编译上传
 * ============================================================================ */

/* PD11 GPIO 路径 - 控制 LED 灯 */
#define GPIO_PD11_PATH  "/sys/class/gpio/PD11"

/*
 * TTC 阈值 (秒)
 *   TTC = 距离(m) / |接近速度|(m/s)
 *   当目标靠近且 TTC 小于此值时，LED 开始闪烁
 *   推荐：车辆盲区监测 3~5秒，人员靠近测试 5~10秒
 */
#define TTC_THRESHOLD   10.0f

/*
 * 距离阈值 (米)
 *   当雷达速度为 0 时（固件限制），用距离直接判断
 *   任一目标距离 <= 此值时触发 LED
 *   推荐：盲区边界 2~5米
 */
#define DIST_THRESHOLD  3

/*
 * LED 闪烁周期 (毫秒)
 *   ON = 亮灯时间，OFF = 灭灯时间
 *   推荐：室外 100~200ms（更醒目），室内 200~500ms
 */
#define LED_BLINK_ON_MS  200
#define LED_BLINK_OFF_MS 200

#define HEAD_CMD        0x58
#define HEAD_REPLY      0x59
#define HEAD_REPORT     0x5A

#define TYPE_BSD        7

#pragma pack(push, 1)
typedef struct {
    int8_t  range_val;
    int8_t  angle_val;
    int8_t  velo_val;
    int8_t  objId;
} bsd_obj_t;

typedef struct {
    uint16_t obj_num;
    uint16_t reserved;
    bsd_obj_t obj[8];
} bsd_det_t;

/* 0x30 查询雷达感应信息结构体 (手册 3.2.6) */
typedef struct {
    uint8_t  is_detected;   /* 综合检测结果 */
    uint8_t  det_result;    /* 检测类型结果 */
    uint16_t range_val;     /* 距离, 单位 mm */
    int16_t  angle_val;     /* 角度, 单位 度 */
    int16_t  velo_val;      /* 速度 */
    uint8_t  reserved[6];   /* 保留 */
    uint8_t  rb_conf;       /* 距离置信度 0~16, <12时距离可能不准 */
} radar_det_info_t;
#pragma pack(pop)

static volatile int g_running = 1;
static int g_fd = -1;
static int g_verbose = 1;
static volatile int g_led_alert = 0;   /* 0=安全, 1=危险(LED闪烁) */
static int g_gpio_fd = -1;

/* ---------- PD11 GPIO 控制 ---------- */
static int gpio_init(void)
{
    int fd = open(GPIO_PD11_PATH "/direction", O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "无法打开 %s/direction: %s\n", GPIO_PD11_PATH, strerror(errno));
        return -1;
    }
    write(fd, "out", 3);
    close(fd);

    g_gpio_fd = open(GPIO_PD11_PATH "/value", O_WRONLY);
    if (g_gpio_fd < 0) {
        fprintf(stderr, "无法打开 %s/value: %s\n", GPIO_PD11_PATH, strerror(errno));
        return -1;
    }
    write(g_gpio_fd, "0", 1);  /* 默认熄灭 */
    return 0;
}

static void gpio_set(int val)
{
    if (g_gpio_fd >= 0) {
        write(g_gpio_fd, val ? "1" : "0", 1);
    }
}

static void gpio_deinit(void)
{
    if (g_gpio_fd >= 0) {
        write(g_gpio_fd, "0", 1);
        close(g_gpio_fd);
        g_gpio_fd = -1;
    }
}

/* ---------- LED 闪烁线程 ---------- */
static void* led_thread(void* arg)
{
    (void)arg;
    while (g_running) {
        if (g_led_alert) {
            gpio_set(1);
            usleep(LED_BLINK_ON_MS * 1000);
            gpio_set(0);
            usleep(LED_BLINK_OFF_MS * 1000);
        } else {
            gpio_set(0);
            usleep(100000);  /* 100ms 轮询 */
        }
    }
    gpio_set(0);
    return NULL;
}

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

static int set_uart(int fd, int baudrate)
{
    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        fprintf(stderr, "tcgetattr failed: %s\n", strerror(errno));
        return -1;
    }
    speed_t speed;
    switch (baudrate) {
        case 9600:   speed = B9600;   break;
        case 115200: speed = B115200; break;
        case 921600: speed = B921600; break;
        default:
            fprintf(stderr, "Unsupported baudrate: %d\n", baudrate);
            return -1;
    }
    cfsetospeed(&tty, speed);
    cfsetispeed(&tty, speed);
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= CREAD | CLOCAL;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY | IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
    tty.c_oflag &= ~OPOST & ~ONLCR;
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;
    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        fprintf(stderr, "tcsetattr failed: %s\n", strerror(errno));
        return -1;
    }
    tcflush(fd, TCIOFLUSH);
    return 0;
}

static uint16_t calc_sum16(const uint8_t *data, int len)
{
    uint32_t sum = 0;
    for (int i = 0; i < len; i++) sum += data[i];
    return (uint16_t)(sum & 0xFFFF);
}

static uint8_t calc_sum8(const uint8_t *data, int len)
{
    uint32_t sum = 0;
    for (int i = 0; i < len; i++) sum += data[i];
    return (uint8_t)(sum & 0xFF);
}

static int send_cmd(int fd, uint8_t group, uint8_t cmd, const uint8_t *params, int param_len)
{
    uint8_t frame[64];
    int idx = 0;
    frame[idx++] = HEAD_CMD;
    uint8_t cmd_byte = (group << 5) | (cmd & 0x1F);
    frame[idx++] = cmd_byte;
    frame[idx++] = (uint8_t)param_len;
    if (params && param_len > 0) {
        memcpy(&frame[idx], params, param_len);
        idx += param_len;
    }
    uint16_t csum = calc_sum16(frame, idx);
    frame[idx++] = (uint8_t)(csum & 0xFF);
    frame[idx++] = (uint8_t)((csum >> 8) & 0xFF);

    if (g_verbose) {
        printf("TX: ");
        for (int i = 0; i < idx; i++) printf("%02X ", frame[i]);
        printf("\n");
    }

    int written = write(fd, frame, idx);
    if (written != idx) {
        fprintf(stderr, "write failed: %s\n", strerror(errno));
        return -1;
    }
    tcdrain(fd);
    return 0;
}

static void flush_rx(int fd)
{
    uint8_t tmp[256];
    int n;
    while ((n = read(fd, tmp, sizeof(tmp))) > 0);
}

static void print_det_info(const radar_det_info_t *info)
{
    if (!info->is_detected) {
        printf("  [雷达感应信息] 未检测到目标\n");
        return;
    }
    printf("  [雷达感应信息] 检测到目标\n");
    printf("    检测结果类型   : 0x%02X (bit0=运动 bit1=微动 bit2=存在)\n", info->det_result);
    printf("    距离           : %u mm (%.2f m)\n", info->range_val, info->range_val / 1000.0);
    printf("    角度           : %d 度\n", info->angle_val);
    printf("    速度           : %d\n", info->velo_val);
    printf("    距离置信度     : %d/16 (%s)\n", info->rb_conf,
           info->rb_conf >= 12 ? "可靠" : "不可靠，距离可能不准");
}

static void process_bsd_report(const bsd_det_t *bsd)
{
    int obj_count = bsd->obj_num;
    if (obj_count > 8) obj_count = 8;

    float min_ttc = 9999.0f;
    int has_approaching = 0;

    printf("[BSD 上报] 检测到 %d 个目标\n", obj_count);

    for (int i = 0; i < obj_count; i++) {
        const bsd_obj_t *o = &bsd->obj[i];
        printf("  目标 ID=%d  距离=%dm  角度=%d°  速度=%dm/s",
               o->objId, o->range_val, o->angle_val, o->velo_val);

        /* TTC (Time To Collision) 计算 */
        if (o->velo_val < 0 && o->range_val > 0) {
            float speed = (float)(-o->velo_val);  /* 接近速度为正值 */
            float ttc = (float)o->range_val / speed;
            printf("  TTC=%.1fs", ttc);
            if (ttc < min_ttc) min_ttc = ttc;
            has_approaching = 1;
        }
        printf("\n");
    }

    /* TTC + 距离双重判断: 更容易触发 */
    int should_alert = 0;
    float alert_ttc = has_approaching ? min_ttc : 9999.0f;

    if (has_approaching && min_ttc < TTC_THRESHOLD) {
        should_alert = 1;  /* TTC 阈值触发 */
    } else if (obj_count > 0) {
        /* 速度为0时: 任一目标距离 <= DIST_THRESHOLD 也触发 */
        for (int i = 0; i < obj_count; i++) {
            if (bsd->obj[i].range_val > 0 && bsd->obj[i].range_val <= DIST_THRESHOLD) {
                should_alert = 1;
                alert_ttc = (float)bsd->obj[i].range_val;  /* 用距离代替TTC显示 */
                break;
            }
        }
    }

    if (should_alert) {
        if (!g_led_alert) {
            printf("  [TTC告警] 目标靠近(TTC=%.1fs)或距离过近(<=%dm), LED开始闪烁!\n",
                   alert_ttc, DIST_THRESHOLD);
        }
        g_led_alert = 1;
    } else {
        if (g_led_alert) {
            printf("  [TTC安全] 目标已远离, LED停止闪烁\n");
        }
        g_led_alert = 0;
    }
}

static int process_report_frame(const uint8_t *payload, int payload_len)
{
    if (payload_len < 1) return -1;
    uint8_t type = payload[0];

    if (type != TYPE_BSD) {
        if (g_verbose) printf("[上报] TYPE=%d (非BSD类型，忽略)\n", type);
        return 0;
    }

    int data_len = payload_len - 1;
    if (data_len < 4) {
        printf("[BSD 上报] 数据过短: %d 字节\n", data_len);
        return -1;
    }

    const uint8_t *data = payload + 1;
    bsd_det_t bsd;
    memset(&bsd, 0, sizeof(bsd));
    bsd.obj_num = (uint16_t)data[0] | ((uint16_t)data[1] << 8);

    int obj_count = bsd.obj_num;
    if (obj_count > 8) obj_count = 8;

    int expected_len = 4 + obj_count * (int)sizeof(bsd_obj_t);
    if (data_len < expected_len) {
        printf("[BSD 上报] 数据不完整: 收到 %d, 需要 %d\n", data_len, expected_len);
        obj_count = (data_len - 4) / (int)sizeof(bsd_obj_t);
        if (obj_count < 0) obj_count = 0;
    }
    for (int i = 0; i < obj_count; i++) {
        int off = 4 + i * (int)sizeof(bsd_obj_t);
        if (off + (int)sizeof(bsd_obj_t) <= data_len) {
            memcpy(&bsd.obj[i], &data[off], sizeof(bsd_obj_t));
        }
    }
    process_bsd_report(&bsd);
    return 0;
}

static void process_reply_frame(const uint8_t *payload, int payload_len)
{
    if (payload_len < 3) return;
    uint8_t cmd_byte = payload[0];
    uint8_t param_len = payload[1];
    const uint8_t *params = &payload[2];
    int grp = cmd_byte >> 5;
    int cmd = cmd_byte & 0x1F;

    if (cmd_byte == 0x30 && param_len >= 15) {
        /* 0x30 雷达感应信息查询回复 — 直接翻译为中文 */
        radar_det_info_t info;
        memcpy(&info, params, sizeof(info));
        print_det_info(&info);
        return;
    }

    printf("RX Reply G%d.0x%02X Len=%d: ", grp, cmd, param_len);
    for (int i = 0; i < param_len && i < payload_len - 2; i++) printf("%02X ", params[i]);

    if (grp == 6 && cmd == 0x11) {
        printf("[打开雷达感应: %s]\n", param_len >= 1 && params[0] == 0 ? "成功" : "失败/未知");
    } else if (grp == 6 && cmd == 0x10) {
        printf("[查询雷达状态: %s]\n", param_len >= 1 && params[0] == 1 ? "已打开" : "已关闭");
    } else if (grp == 7 && cmd == 0x1E) {
        if (param_len >= 8) {
            printf("[版本] SW=%d.%d.%d 客户=%d.%d CI=%d.%d 算法=%d\n",
                   params[0], params[1], params[2], params[3], params[4],
                   params[5], params[6], params[7]);
        } else {
            printf("[版本]\n");
        }
    } else if (grp == 0 && cmd == 0x02) {
        printf("[设置感应等级: %s]\n", param_len >= 1 && params[0] == 0 ? "成功" : "失败/未知");
    } else if (grp == 0 && cmd == 0x03) {
        printf("[当前感应等级: %d]\n", param_len >= 1 ? params[0] : -1);
    } else if (grp == 1 && cmd == 0x10) {
        /* 0x30 已被上面处理，这里不会走到 */
        printf("[OK]\n");
    } else {
        printf("[OK]\n");
    }
}

static int process_frame(const uint8_t *frame, int frame_len)
{
    if (frame_len < 4) return -1;
    uint8_t head = frame[0];

    if (head == HEAD_REPORT) {
        uint8_t len = frame[1];
        if (2 + len + 1 > frame_len) return -1;
        uint8_t cs = calc_sum8(frame, 2 + len);
        if (cs != frame[2 + len]) {
            if (g_verbose) printf("[上报] 校验和错误: 计算=0x%02X 接收=0x%02X\n", cs, frame[2 + len]);
            return -1;
        }
        return process_report_frame(&frame[2], len + 1);
    } else if (head == HEAD_REPLY) {
        if (frame_len < 5) return -1;
        uint8_t cmd_byte = frame[1];
        uint8_t param_len = frame[2];
        if (3 + param_len + 2 > frame_len) return -1;
        uint16_t recv_cs = (uint16_t)frame[3 + param_len] | ((uint16_t)frame[3 + param_len + 1] << 8);
        uint16_t calc_cs = calc_sum16(frame, 3 + param_len);
        if (calc_cs != recv_cs) {
            if (g_verbose) printf("[回复] 校验和错误: 计算=0x%04X 接收=0x%04X\n", calc_cs, recv_cs);
            return -1;
        }
        uint8_t payload[256];
        payload[0] = cmd_byte;
        payload[1] = param_len;
        if (param_len > 0) memcpy(&payload[2], &frame[3], param_len);
        process_reply_frame(payload, 2 + param_len);
        return 0;
    }
    return -1;
}

static int wait_reply(int fd, int timeout_ms)
{
    uint8_t buf[512];
    int buf_len = 0;
    struct timeval start, now;
    gettimeofday(&start, NULL);

    while (g_running) {
        gettimeofday(&now, NULL);
        int elapsed = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_usec - start.tv_usec) / 1000;
        if (elapsed >= timeout_ms) break;

        int remain = timeout_ms - elapsed;
        struct timeval tv = { remain / 1000, (remain % 1000) * 1000 };
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        int ret = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (ret <= 0) break;
        if (!FD_ISSET(fd, &rfds)) break;

        int n = read(fd, buf + buf_len, sizeof(buf) - buf_len);
        if (n <= 0) break;
        buf_len += n;

        while (buf_len >= 4) {
            uint8_t head = buf[0];
            int frame_total = -1;
            if (head == HEAD_REPLY) {
                if (buf_len < 3) break;
                uint8_t plen = buf[2];
                frame_total = 5 + plen;
            } else if (head == HEAD_REPORT) {
                if (buf_len < 3) break;
                uint8_t plen = buf[1];
                frame_total = 3 + plen + 1;
            } else {
                memmove(buf, buf + 1, buf_len - 1);
                buf_len--;
                continue;
            }
            if (frame_total > (int)sizeof(buf) || frame_total > buf_len) break;

            int consumed = frame_total;
            uint8_t frame[512];
            if (consumed <= 512) {
                memcpy(frame, buf, consumed);
                process_frame(frame, consumed);
            }
            memmove(buf, buf + consumed, buf_len - consumed);
            buf_len -= consumed;
        }
    }
    return 0;
}

static int send_with_reply(int fd, uint8_t grp, uint8_t cmd, const uint8_t *params, int plen, int timeout_ms)
{
    flush_rx(fd);
    if (send_cmd(fd, grp, cmd, params, plen) != 0) return -1;
    return wait_reply(fd, timeout_ms);
}

int main(int argc, char *argv[])
{
    const char *device = UART_DEVICE;
    int baudrate = BAUDRATE;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("MS60-1211S80M-BSD 雷达初始化 + 持续接收 + PD11 LED TTC告警\n");
    printf("开发板: STM32 USART1 (PG14/PG15 AF6) -> %s @ %d\n", device, baudrate);
    printf("LED: PD11 GPIO, TTC阈值: %.1fs\n\n", TTC_THRESHOLD);

    /* 初始化 PD11 GPIO */
    if (gpio_init() != 0) {
        fprintf(stderr, "GPIO 初始化失败, LED 功能不可用\n");
    } else {
        printf("PD11 GPIO 初始化成功.\n");
    }

    /* 创建 LED 闪烁线程 */
    pthread_t led_tid;
    if (pthread_create(&led_tid, NULL, led_thread, NULL) != 0) {
        fprintf(stderr, "LED 线程创建失败\n");
    }

    g_fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (g_fd < 0) {
        fprintf(stderr, "无法打开 %s: %s\n", device, strerror(errno));
        gpio_deinit();
        return 1;
    }

    if (set_uart(g_fd, baudrate) != 0) {
        close(g_fd);
        return 1;
    }
    printf("串口配置成功.\n\n");

    printf("步骤1: 查询版本...\n");
    send_with_reply(g_fd, 7, 0x1E, NULL, 0, 1000);
    usleep(200000);

    printf("\n步骤2: 查询当前感应等级...\n");
    send_with_reply(g_fd, 0, 0x03, NULL, 0, 1000);
    usleep(200000);

    printf("\n步骤3: 打开雷达感应...\n");
    send_with_reply(g_fd, 6, 0x11, (uint8_t[]){0x01}, 1, 1000);
    usleep(200000);

    printf("\n步骤4: 设置感应等级为 0 (最灵敏)...\n");
    send_with_reply(g_fd, 0, 0x02, (uint8_t[]){0x00}, 1, 1000);
    usleep(200000);

    printf("\n步骤5: 设置检测最远距离为 5000cm (50m)...\n");
    send_with_reply(g_fd, 6, 0x12, (uint8_t[]){0x88, 0x13}, 2, 1000);
    usleep(200000);

    printf("\n步骤6: 查询雷达状态...\n");
    send_with_reply(g_fd, 6, 0x10, NULL, 0, 1000);
    usleep(200000);

    printf("\n=== 初始化完成. 进入接收循环 (按 Ctrl+C 退出) ===\n");
    printf("提示: 每 3 秒自动查询一次雷达感应信息.\n\n");

    uint8_t rx_buf[512];
    int rx_len = 0;

    int poll_cnt = 0;
    while (g_running) {
        fd_set rfds;
        struct timeval tv = {1, 0};
        FD_ZERO(&rfds);
        FD_SET(g_fd, &rfds);
        int ret = select(g_fd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* 每 3 秒查询一次感应信息 (0x30) */
        poll_cnt++;
        if (poll_cnt >= 3) {
            poll_cnt = 0;
            send_cmd(g_fd, 1, 0x10, NULL, 0);
        }

        if (ret > 0 && FD_ISSET(g_fd, &rfds)) {
            int n = read(g_fd, rx_buf + rx_len, sizeof(rx_buf) - rx_len);
            if (n > 0) rx_len += n;

            while (rx_len >= 4) {
                uint8_t head = rx_buf[0];
                int frame_total = -1;
                if (head == HEAD_REPORT) {
                    if (rx_len < 3) break;
                    frame_total = 2 + rx_buf[1] + 1;  /* head(1) + len(1) + data(N) + cs(1) = N+3 */
                } else if (head == HEAD_REPLY) {
                    if (rx_len < 3) break;
                    frame_total = 5 + rx_buf[2];
                } else {
                    int i = 1;
                    for (; i < rx_len; i++) {
                        if (rx_buf[i] == HEAD_REPORT || rx_buf[i] == HEAD_REPLY || rx_buf[i] == HEAD_CMD)
                            break;
                    }
                    if (i < rx_len) {
                        memmove(rx_buf, rx_buf + i, rx_len - i);
                        rx_len -= i;
                    } else {
                        rx_len = 0;
                    }
                    continue;
                }
                if (frame_total > (int)sizeof(rx_buf) || frame_total > rx_len) break;

                uint8_t frame[512];
                memcpy(frame, rx_buf, frame_total);
                process_frame(frame, frame_total);
                memmove(rx_buf, rx_buf + frame_total, rx_len - frame_total);
                rx_len -= frame_total;
            }
            if (rx_len >= (int)sizeof(rx_buf)) rx_len = 0;
        }
    }

    printf("\n雷达程序已停止\n");
    close(g_fd);

    /* 等待 LED 线程退出 */
    pthread_join(led_tid, NULL);
    gpio_deinit();

    printf("PD11 GPIO 已关闭\n");
    return 0;
}
