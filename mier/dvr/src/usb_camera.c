#include "usb_camera.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

struct usb_camera_ctx {
    int    fd;
    int    width;
    int    height;
    int    fps;
    int    buf_count;
    int    pixelformat;
    int    frame_size;
    int    bpp;
    char   fmt_name[32];
    void  *buffers[4];
    size_t buf_lengths[4];
};

usb_camera_t *usb_camera_open(const char *device, int width, int height, int fps)
{
    usb_camera_t *ctx = calloc(1, sizeof(usb_camera_t));
    if (!ctx) return NULL;

    ctx->fd = open(device, O_RDWR);
    if (ctx->fd < 0) {
        fprintf(stderr, "[USB_CAM] Cannot open %s: %s\n", device, strerror(errno));
        free(ctx);
        return NULL;
    }

    struct v4l2_capability cap;
    if (ioctl(ctx->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        fprintf(stderr, "[USB_CAM] VIDIOC_QUERYCAP failed: %s\n", strerror(errno));
        goto fail;
    }
    printf("[USB_CAM] Device: %s, driver: %s, bus: %s\n",
           cap.card, cap.driver,
           (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? "usb" : "unknown");

    /* USB UVC 摄像头支持的格式，按优先级排列 */
    static const uint32_t try_fmts[] = {
        V4L2_PIX_FMT_MJPEG,   /* MJPEG - USB摄像头最常用，压缩格式 */
        V4L2_PIX_FMT_YUYV,    /* YUYV - 原始格式 */
        V4L2_PIX_FMT_RGB565,  /* RGB565 */
        V4L2_PIX_FMT_RGB24,   /* RGB24 */
        V4L2_PIX_FMT_NV12,    /* NV12 */
        V4L2_PIX_FMT_YUV420,  /* YUV420 */
    };
    static const char *try_names[] = { "MJPEG", "YUYV", "RGB565", "RGB24", "NV12", "YUV420" };
    static const int   try_bpp[]   = { 0,      2,      2,       3,       1,      1 };

    struct v4l2_format fmt;
    int ok = 0;
    for (int i = 0; i < (int)(sizeof(try_fmts)/sizeof(try_fmts[0])); i++) {
        memset(&fmt, 0, sizeof(fmt));
        fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width       = width;
        fmt.fmt.pix.height      = height;
        fmt.fmt.pix.pixelformat = try_fmts[i];
        fmt.fmt.pix.field       = V4L2_FIELD_NONE;

        if (ioctl(ctx->fd, VIDIOC_S_FMT, &fmt) == 0) {
            ctx->pixelformat = try_fmts[i];
            ctx->bpp         = try_bpp[i];
            strncpy(ctx->fmt_name, try_names[i], sizeof(ctx->fmt_name) - 1);
            printf("[USB_CAM] Format: %dx%d %s\n",
                   fmt.fmt.pix.width, fmt.fmt.pix.height, try_names[i]);
            ok = 1;
            break;
        }
    }

    if (!ok) {
        fprintf(stderr, "[USB_CAM] No supported format found\n");
        goto fail;
    }

    ctx->width      = fmt.fmt.pix.width;
    ctx->height     = fmt.fmt.pix.height;
    ctx->frame_size = fmt.fmt.pix.sizeimage;
    ctx->fps        = fps;

    printf("[USB_CAM] Frame size: %d bytes\n", ctx->frame_size);

    /* 设置帧率 */
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator   = 1;
    parm.parm.capture.timeperframe.denominator = fps;
    if (ioctl(ctx->fd, VIDIOC_S_PARM, &parm) == 0) {
        printf("[USB_CAM] FPS set: %d/%d\n",
               parm.parm.capture.timeperframe.denominator,
               parm.parm.capture.timeperframe.numerator);
    }

    /* 申请缓冲区 */
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = 4;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(ctx->fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "[USB_CAM] VIDIOC_REQBUFS failed: %s\n", strerror(errno));
        goto fail;
    }
    ctx->buf_count = req.count;
    printf("[USB_CAM] Buffers allocated: %d\n", ctx->buf_count);

    for (int i = 0; i < ctx->buf_count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (ioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            fprintf(stderr, "[USB_CAM] VIDIOC_QUERYBUF %d failed\n", i);
            goto fail;
        }
        ctx->buf_lengths[i] = buf.length;
        ctx->buffers[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                               MAP_SHARED, ctx->fd, buf.m.offset);
        if (ctx->buffers[i] == MAP_FAILED) {
            fprintf(stderr, "[USB_CAM] mmap %d failed\n", i);
            goto fail;
        }
    }

    /* 入队 */
    for (int i = 0; i < ctx->buf_count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (ioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
            fprintf(stderr, "[USB_CAM] VIDIOC_QBUF %d failed\n", i);
            goto fail;
        }
    }

    /* 开始采集 */
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(ctx->fd, VIDIOC_STREAMON, &type) < 0) {
        fprintf(stderr, "[USB_CAM] VIDIOC_STREAMON failed: %s\n", strerror(errno));
        goto fail;
    }

    printf("[USB_CAM] Streaming started: %dx%d @ %d fps, format=%s\n",
           ctx->width, ctx->height, ctx->fps, ctx->fmt_name);
    return ctx;

fail:
    close(ctx->fd);
    free(ctx);
    return NULL;
}

void usb_camera_close(usb_camera_t *ctx)
{
    if (!ctx) return;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < ctx->buf_count; i++) {
        munmap(ctx->buffers[i], ctx->buf_lengths[i]);
    }
    close(ctx->fd);
    free(ctx);
    printf("[USB_CAM] Closed\n");
}

int usb_camera_grab_frame(usb_camera_t *ctx, uint8_t *buffer, int buf_size, int64_t *ts_us)
{
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(ctx->fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN) return 0;
        fprintf(stderr, "[USB_CAM] VIDIOC_DQBUF failed: %s\n", strerror(errno));
        return -1;
    }

    int copy_size = (int)buf.bytesused < buf_size ? (int)buf.bytesused : buf_size;
    memcpy(buffer, ctx->buffers[buf.index], copy_size);

    /* 使用 gettimeofday 获取相对时间戳 */
    if (ts_us) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        *ts_us = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
    }

    if (ioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
        fprintf(stderr, "[USB_CAM] VIDIOC_QBUF failed: %s\n", strerror(errno));
        return -1;
    }

    return copy_size;
}

int         usb_camera_get_fd(const usb_camera_t *ctx)             { return ctx ? ctx->fd : -1; }
int         usb_camera_get_width(const usb_camera_t *ctx)          { return ctx ? ctx->width : 0; }
int         usb_camera_get_height(const usb_camera_t *ctx)         { return ctx ? ctx->height : 0; }
int         usb_camera_get_pixelformat(const usb_camera_t *ctx)    { return ctx ? ctx->pixelformat : 0; }
int         usb_camera_get_frame_size(const usb_camera_t *ctx)     { return ctx ? ctx->frame_size : 0; }
const char *usb_camera_get_pixelformat_name(const usb_camera_t *ctx) { return ctx ? ctx->fmt_name : "unknown"; }