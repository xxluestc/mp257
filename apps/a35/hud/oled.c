#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <stdint.h>
#include <errno.h>
#include "oled.h"

#define OLED_CMD 0x00
#define OLED_DAT 0x40

static int i2c_fd = -1;
static uint8_t oled_buf[8][128];

// ==================== I2C ====================
static int oled_write_byte(uint8_t control, uint8_t value) {
    const uint8_t buf[2] = {control, value};
    ssize_t written;
    do {
        written = write(i2c_fd, buf, sizeof(buf));
    } while (written < 0 && errno == EINTR);
    if (written == (ssize_t)sizeof(buf))
        return 0;
    // Each I2C write is a complete control+data transaction. A short write
    // cannot be repaired by sending its remainder as a separate transaction.
    if (written >= 0)
        errno = EIO;
    return -1;
}

static int oled_write_cmd(uint8_t cmd) {
    return oled_write_byte(OLED_CMD, cmd);
}

static int oled_flush(void) {
    for (int page = 0; page < 8; page++) {
        if (oled_write_cmd(0xB0 + page) || oled_write_cmd(0x00) || oled_write_cmd(0x10))
            return -1;
        for (int col = 0; col < 128; col++) {
            if (oled_write_byte(OLED_DAT, oled_buf[page][col]))
                return -1;
        }
    }
    return 0;
}

static void oled_clear_buf(void) {
    memset(oled_buf, 0, sizeof(oled_buf));
}

static void oled_set_pixel(int x, int y) {
    if (x < 0 || x >= 128 || y < 0 || y >= 64)
        return;
    oled_buf[y / 8][x] |= (1 << (y % 8));
}

// ==================== 6x8 列模式字符库 ====================
static const uint8_t font_space[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t font_A[6] = {0x7E, 0x11, 0x11, 0x11, 0x7E, 0x00};
static const uint8_t font_C[6] = {0x3E, 0x41, 0x41, 0x41, 0x22, 0x00};
static const uint8_t font_D[6] = {0x7F, 0x41, 0x41, 0x22, 0x1C, 0x00};
static const uint8_t font_I[6] = {0x00, 0x41, 0x7F, 0x41, 0x00, 0x00};
static const uint8_t font_N[6] = {0x7F, 0x04, 0x08, 0x10, 0x7F, 0x00};
static const uint8_t font_O[6] = {0x3E, 0x41, 0x41, 0x41, 0x3E, 0x00};
static const uint8_t font_R[6] = {0x7F, 0x09, 0x19, 0x29, 0x46, 0x00};
static const uint8_t font_S[6] = {0x46, 0x49, 0x49, 0x49, 0x31, 0x00};
static const uint8_t font_T[6] = {0x01, 0x01, 0x7F, 0x01, 0x01, 0x00};
static const uint8_t font_m[6] = {0x7C, 0x04, 0x18, 0x04, 0x78, 0x00};
static const uint8_t font_k[6] = {0x7F, 0x08, 0x14, 0x22, 0x00, 0x00};
static const uint8_t font_dot[6] = {0x00, 0x00, 0x00, 0x18, 0x18, 0x00}; // '.'

// 数字 0~9
static const uint8_t font_digit[10][6] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E, 0x00}, // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00, 0x00}, // 1
    {0x42, 0x61, 0x51, 0x49, 0x46, 0x00}, // 2
    {0x21, 0x41, 0x45, 0x4B, 0x31, 0x00}, // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10, 0x00}, // 4
    {0x27, 0x45, 0x45, 0x45, 0x39, 0x00}, // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30, 0x00}, // 6
    {0x01, 0x71, 0x09, 0x05, 0x03, 0x00}, // 7
    {0x36, 0x49, 0x49, 0x49, 0x36, 0x00}, // 8
    {0x06, 0x49, 0x49, 0x29, 0x1E, 0x00}  // 9
};

static const uint8_t *get_font(char c) {
    if (c >= '0' && c <= '9') {
        return font_digit[c - '0'];
    }
    switch (c) {
    case ' ':
        return font_space;
    case 'A':
        return font_A;
    case 'C':
        return font_C;
    case 'D':
        return font_D;
    case 'I':
        return font_I;
    case 'N':
        return font_N;
    case 'O':
        return font_O;
    case 'R':
        return font_R;
    case 'S':
        return font_S;
    case 'T':
        return font_T;
    case 'm':
        return font_m;
    case 'k':
        return font_k;
    case '.':
        return font_dot;
    default:
        return font_space;
    }
}

static void oled_draw_char(int x, int y, char c, int scale) {
    const uint8_t *glyph = get_font(c);
    for (int col = 0; col < 6; col++) {
        uint8_t column = glyph[col];
        for (int row = 0; row < 8; row++) {
            if (column & (1 << row)) {
                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) {
                        oled_set_pixel(x + col * scale + dx, y + row * scale + dy);
                    }
                }
            }
        }
    }
}

static void oled_draw_string(int x, int y, const char *str, int scale) {
    while (*str) {
        oled_draw_char(x, y, *str, scale);
        x += 6 * scale;
        str++;
    }
}

// 大数字绘制（用于 <1000m 的距离数字）
static void oled_draw_big_number(int x, int y, int num, int scale) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", num);

    for (char *p = buf; *p; p++) {
        int digit = *p - '0';
        if (digit < 0 || digit > 9)
            continue;

        const uint8_t *glyph = font_digit[digit];
        for (int col = 0; col < 6; col++) {
            uint8_t column = glyph[col];
            for (int row = 0; row < 8; row++) {
                if (column & (1 << row)) {
                    for (int dy = 0; dy < scale; dy++) {
                        for (int dx = 0; dx < scale; dx++) {
                            oled_set_pixel(x + col * scale + dx, y + row * scale + dy);
                        }
                    }
                }
            }
        }
        x += 6 * scale + 2;
    }
}

// ==================== 箭头（8x8 位图） ====================
static const uint8_t arrow_up[8] = {0x18, 0x3C, 0x7E, 0x18, 0x18, 0x18, 0x18, 0x00};
static const uint8_t arrow_down[8] = {0x18, 0x18, 0x18, 0x18, 0x7E, 0x3C, 0x18, 0x00};
static const uint8_t arrow_left[8] = {0x10, 0x30, 0x7E, 0xFE, 0x7E, 0x30, 0x10, 0x00};
static const uint8_t arrow_right[8] = {0x08, 0x0C, 0x7E, 0x7F, 0x7E, 0x0C, 0x08, 0x00};

static void oled_draw_arrow(int x, int y, const uint8_t bmp[8], int scale) {
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            if (bmp[row] & (1 << (7 - col))) {
                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) {
                        oled_set_pixel(x + col * scale + dx, y + row * scale + dy);
                    }
                }
            }
        }
    }
}

static void oled_draw_arrow_by_turn(int x, int y, int turn, int scale) {
    const uint8_t *arrow = arrow_up;
    switch (turn) {
    case 2:
        arrow = arrow_left;
        break;
    case 3:
        arrow = arrow_right;
        break;
    case 8:
        arrow = arrow_down;
        break;
    case 9:
        arrow = arrow_up;
        break;
    default:
        arrow = arrow_up;
        break;
    }
    oled_draw_arrow(x, y, arrow, scale);
}

// ==================== OLED 初始化 ====================
static int oled_init_sequence(void) {
    const uint8_t commands[] = {0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D,
                                0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12, 0x81, 0xCF,
                                0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF};
    for (size_t index = 0; index < sizeof(commands); ++index)
        if (oled_write_cmd(commands[index]))
            return -1;
    return 0;
}

int oled_init(void) {
    if (i2c_fd >= 0)
        return 0;
    i2c_fd = open(OLED_I2C_DEV, O_RDWR | O_CLOEXEC);
    if (i2c_fd < 0)
        return -1;

    if (ioctl(i2c_fd, I2C_SLAVE, OLED_ADDR) < 0) {
        close(i2c_fd);
        i2c_fd = -1;
        return -1;
    }

    oled_clear_buf();
    oled_draw_string(20, 26, "NO DATA", 2); // 保持不动
    if (oled_init_sequence() || oled_flush()) {
        const int saved_errno = errno;
        oled_close();
        errno = saved_errno;
        return -1;
    }
    return 0;
}

int oled_clear(void) {
    oled_clear_buf();
    return oled_flush();
}

// ==================== 主显示函数（右下对齐，底部基线对齐，小字体，km 自动转换） ====================
int oled_show_nav(const NavData *nav, int has_signal) {
    oled_clear_buf();

    if (!has_signal || nav == NULL) {
        oled_draw_string(20, 26, "NO DATA", 2);
        return oled_flush();
    }

    const int RIGHT_EDGE = 97; // 向左移动 25 像素
    const int BASELINE_Y = 52; // 向下移动 6 像素
    const int CHAR_HEIGHT = 8; // 字符高度（scale=1）
    const int ARROW_SCALE = 2; // 箭头 16x16
    const int DIR_SCALE = 1;   // DIR 文字 6x8
    const int LABEL_SCALE = 1; // DIST 标签
    const int DIST_SCALE = 1;  // 距离字符串/数字大小
    const int GAP_UPPER = 4;   // DIR 与箭头间距
    const int GAP_LABEL = 4;   // DIST 与数字间距

    // 下半行文本的绘制起点（底部对齐）：y = BASELINE_Y - CHAR_HEIGHT*scale + 1
    int text_y = BASELINE_Y - CHAR_HEIGHT * DIST_SCALE + 1; // = 46 - 8 + 1 = 39

    // ---- 上半行：DIR + 箭头 ----
    int arrow_w = 8 * ARROW_SCALE;      // 16
    int dir_w = 3 * 6 * DIR_SCALE;      // 18
    int arrow_x = RIGHT_EDGE - arrow_w; // 106
    int arrow_y = 20;                   // 向下移动 6 像素
    int dir_x = arrow_x - GAP_UPPER - dir_w;
    int dir_y = arrow_y + (arrow_w - 8) / 2; // DIR 垂直居中

    oled_draw_string(dir_x, dir_y, "DIR", DIR_SCALE);
    oled_draw_arrow_by_turn(arrow_x, arrow_y, nav->turn, ARROW_SCALE);

    // ---- 下半行：DIST + 距离/字符串 + 单位 ----
    int dist_label_w = 4 * 6 * LABEL_SCALE; // 24

    if (nav->distance >= 1000) {
        // 转换为 km，保留两位小数
        int km_int = nav->distance / 1000;
        int remain = nav->distance % 1000;
        int km_frac = (remain * 100 + 500) / 1000; // 四舍五入
        if (km_frac >= 100) {
            km_frac = 0;
            km_int++;
        }

        char dist_str[16];
        snprintf(dist_str, sizeof(dist_str), "%d.%02d", km_int, km_frac);
        int str_len = strlen(dist_str);
        int dist_str_w = str_len * 6 * DIST_SCALE; // 每个字符 6px

        char unit_str[] = "km";
        int unit_w = 2 * 6 * DIST_SCALE; // 12

        int total_w = dist_str_w + 2 + unit_w;
        int dist_num_x = RIGHT_EDGE - total_w;
        int unit_x = dist_num_x + dist_str_w + 2;

        oled_draw_string(dist_num_x, text_y, dist_str, DIST_SCALE);
        oled_draw_string(unit_x, text_y, unit_str, DIST_SCALE);

        int label_x = dist_num_x - GAP_LABEL - dist_label_w;
        oled_draw_string(label_x, text_y, "DIST", LABEL_SCALE);
    } else {
        // 小于 1000m：大数字 + "m"
        char dist_buf[16];
        snprintf(dist_buf, sizeof(dist_buf), "%d", nav->distance);
        int digit_count = strlen(dist_buf);
        int dist_num_w = digit_count * 8 - 2; // 每个数字宽 8px（6 + 2 间距），减去末尾多余 2

        char unit_str[] = "m";
        int unit_w = 1 * 6 * DIST_SCALE; // 6

        int total_w = dist_num_w + 2 + unit_w;
        int dist_num_x = RIGHT_EDGE - total_w;
        int unit_x = dist_num_x + dist_num_w + 2;

        oled_draw_big_number(dist_num_x, text_y, nav->distance, DIST_SCALE);
        oled_draw_string(unit_x, text_y, unit_str, DIST_SCALE);

        int label_x = dist_num_x - GAP_LABEL - dist_label_w;
        oled_draw_string(label_x, text_y, "DIST", LABEL_SCALE);
    }

    return oled_flush();
}

void oled_close(void) {
    if (i2c_fd >= 0) {
        close(i2c_fd);
        i2c_fd = -1;
    }
}
