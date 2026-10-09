#ifndef OLED_H
#define OLED_H

#include "common.h" // 使用 common.h 中的 NavData 定义

int oled_init(void);
// Caller serializes device access; 0 succeeds, -1 reports an I2C error via errno.
int oled_clear(void);
int oled_show_nav(const NavData *nav, int has_signal);
void oled_close(void);

#endif
