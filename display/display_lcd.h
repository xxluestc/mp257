#ifndef DISPLAY_LCD_H
#define DISPLAY_LCD_H

#include "dvr_types.h"

typedef struct display_ctx display_ctx_t;

display_ctx_t *display_open(int width, int height);
void           display_close(display_ctx_t *ctx);
int            display_show_frame(display_ctx_t *ctx, const uint8_t *data, int width, int height);
int            display_get_fd(const display_ctx_t *ctx);
void           display_set_source_format(display_ctx_t *ctx, int pixelformat, int bpp);

#endif
