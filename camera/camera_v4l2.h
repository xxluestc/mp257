#ifndef CAMERA_V4L2_H
#define CAMERA_V4L2_H

#include <stdint.h>
#include <time.h>

typedef struct camera_ctx camera_ctx_t;

camera_ctx_t *camera_open(const char *device, int width, int height, int fps);
void         camera_close(camera_ctx_t *ctx);

int  camera_grab_frame(camera_ctx_t *ctx, uint8_t *buffer, int buf_size, time_t *ts);
int  camera_get_fd(const camera_ctx_t *ctx);
int  camera_get_width(const camera_ctx_t *ctx);
int  camera_get_height(const camera_ctx_t *ctx);
int  camera_get_pixelformat(const camera_ctx_t *ctx);
int  camera_get_frame_size(const camera_ctx_t *ctx);
int  camera_get_bpp(const camera_ctx_t *ctx);

#endif
