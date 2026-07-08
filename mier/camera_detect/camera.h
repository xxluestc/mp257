#ifndef CAMERA_H
#define CAMERA_H

#include <stdint.h>
#include <linux/videodev2.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int fd;
    int width;
    int height;
    int pixelformat;   /* V4L2_PIX_FMT_MJPEG, YUYV, etc. */
    unsigned int buf_size;
    int buf_count;
    void **buffers;    /* mmap'd buffers */
    unsigned int *buf_lengths;
} camera_t;

int camera_open(camera_t *cam, const char *device, int width, int height);
int camera_start(camera_t *cam);
int camera_capture(camera_t *cam, uint8_t **out_buf, unsigned int *out_len);
void camera_stop(camera_t *cam);
void camera_close(camera_t *cam);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_H */