#include "camera_v4l2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

struct camera_ctx {
    int    fd;
    int    width;
    int    height;
    int    fps;
    int    buf_count;
    int    pixelformat;
    int    frame_size;
    int    bpp;
    void  *buffers[3];
    size_t buf_lengths[3];
};

static void rgb565_to_rgb24(const uint8_t *src, uint8_t *dst, int pixels)
{
    for (int i = 0; i < pixels; i++) {
        uint16_t val = src[i * 2] | (src[i * 2 + 1] << 8);
        uint8_t r = (val >> 11) & 0x1f;
        uint8_t g = (val >> 5) & 0x3f;
        uint8_t b = val & 0x1f;
        dst[i * 3]     = (r << 3) | (r >> 2);
        dst[i * 3 + 1] = (g << 2) | (g >> 4);
        dst[i * 3 + 2] = (b << 3) | (b >> 2);
    }
}

camera_ctx_t *camera_open(const char *device, int width, int height, int fps)
{
    camera_ctx_t *ctx = calloc(1, sizeof(camera_ctx_t));
    if (!ctx) return NULL;

    ctx->fd = open(device, O_RDWR);
    if (ctx->fd < 0) {
        fprintf(stderr, "[CAMERA] Cannot open %s: %s\n", device, strerror(errno));
        free(ctx);
        return NULL;
    }

    struct v4l2_capability cap;
    if (ioctl(ctx->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        fprintf(stderr, "[CAMERA] VIDIOC_QUERYCAP failed: %s\n", strerror(errno));
        goto fail;
    }
    printf("[CAMERA] Device: %s, driver: %s\n", cap.card, cap.driver);

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type           = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width  = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.field  = V4L2_FIELD_NONE;

    static const uint32_t try_fmts[] = {
        V4L2_PIX_FMT_RGB565,
        V4L2_PIX_FMT_RGB24,
        V4L2_PIX_FMT_NV12,
    };
    static const char *try_names[] = { "RGB565", "RGB24", "NV12" };
    static const int try_bpp[]     = { 2,       3,      1.5   };

    int ok = 0;
    for (int i = 0; i < 3; i++) {
        fmt.fmt.pix.pixelformat = try_fmts[i];
        fmt.fmt.pix.width  = width;
        fmt.fmt.pix.height = height;
        if (ioctl(ctx->fd, VIDIOC_S_FMT, &fmt) == 0) {
            ctx->pixelformat = try_fmts[i];
            ctx->bpp         = try_bpp[i];
            printf("[CAMERA] Format: %dx%d %s (req: %dx%d)\n",
                   fmt.fmt.pix.width, fmt.fmt.pix.height, try_names[i],
                   width, height);
            ok = 1;
            break;
        }
        fprintf(stderr, "[CAMERA] %s not supported, trying next...\n", try_names[i]);
    }

    if (!ok) {
        fprintf(stderr, "[CAMERA] No supported format found\n");
        goto fail;
    }

    ctx->width      = fmt.fmt.pix.width;
    ctx->height     = fmt.fmt.pix.height;
    ctx->frame_size = fmt.fmt.pix.sizeimage;

    {
        struct v4l2_format verify;
        memset(&verify, 0, sizeof(verify));
        verify.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(ctx->fd, VIDIOC_G_FMT, &verify);
        printf("[CAMERA] Verified: %dx%d, pixelformat=0x%x, bytesperline=%d, sizeimage=%d\n",
               verify.fmt.pix.width, verify.fmt.pix.height,
               verify.fmt.pix.pixelformat,
               verify.fmt.pix.bytesperline, verify.fmt.pix.sizeimage);
    }

    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator   = 1;
    parm.parm.capture.timeperframe.denominator = fps;
    ioctl(ctx->fd, VIDIOC_S_PARM, &parm);

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = 3;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(ctx->fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "[CAMERA] VIDIOC_REQBUFS failed: %s\n", strerror(errno));
        goto fail;
    }
    ctx->buf_count = req.count;

    for (int i = 0; i < ctx->buf_count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (ioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            fprintf(stderr, "[CAMERA] VIDIOC_QUERYBUF %d failed\n", i);
            goto fail;
        }
        ctx->buf_lengths[i] = buf.length;
        ctx->buffers[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                               MAP_SHARED, ctx->fd, buf.m.offset);
        if (ctx->buffers[i] == MAP_FAILED) {
            fprintf(stderr, "[CAMERA] mmap %d failed\n", i);
            goto fail;
        }
    }

    for (int i = 0; i < ctx->buf_count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (ioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
            fprintf(stderr, "[CAMERA] VIDIOC_QBUF %d failed\n", i);
            goto fail;
        }
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(ctx->fd, VIDIOC_STREAMON, &type) < 0) {
        fprintf(stderr, "[CAMERA] VIDIOC_STREAMON failed: %s\n", strerror(errno));
        goto fail;
    }

    system("/usr/local/demo/bin/dcmipp-isp-ctrl -i0");
    system("/usr/local/demo/bin/dcmipp-isp-ctrl -i0 -g > /dev/null");
    printf("[CAMERA] ISP control configured\n");

    printf("[CAMERA] Streaming: %dx%d @ %d fps, format=%s, bpp=%d, frame_size=%d\n",
           ctx->width, ctx->height, fps,
           ctx->pixelformat == V4L2_PIX_FMT_RGB565 ? "RGB565" :
           ctx->pixelformat == V4L2_PIX_FMT_RGB24 ? "RGB24" : "Other",
           ctx->bpp, ctx->frame_size);
    return ctx;

fail:
    close(ctx->fd);
    free(ctx);
    return NULL;
}

void camera_close(camera_ctx_t *ctx)
{
    if (!ctx) return;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < ctx->buf_count; i++) {
        munmap(ctx->buffers[i], ctx->buf_lengths[i]);
    }
    close(ctx->fd);
    free(ctx);
    printf("[CAMERA] Closed\n");
}

int camera_grab_frame(camera_ctx_t *ctx, uint8_t *buffer, int buf_size, time_t *ts)
{
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(ctx->fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN) return 0;
        fprintf(stderr, "[CAMERA] VIDIOC_DQBUF failed: %s\n", strerror(errno));
        return -1;
    }

    int copy_size = (int)buf.bytesused < buf_size ? (int)buf.bytesused : buf_size;

    if (ctx->pixelformat == V4L2_PIX_FMT_RGB565) {
        memcpy(buffer, ctx->buffers[buf.index], copy_size);
    } else if (ctx->pixelformat == V4L2_PIX_FMT_RGB24) {
        memcpy(buffer, ctx->buffers[buf.index], copy_size);
    } else {
        memcpy(buffer, ctx->buffers[buf.index], copy_size);
    }

    if (ts) *ts = time(NULL);

    if (ioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
        fprintf(stderr, "[CAMERA] VIDIOC_QBUF failed: %s\n", strerror(errno));
        return -1;
    }

    return copy_size;
}

void camera_convert_to_rgb24(const camera_ctx_t *ctx, const uint8_t *src, uint8_t *dst)
{
    if (!ctx || !src || !dst) return;
    if (ctx->pixelformat == V4L2_PIX_FMT_RGB565) {
        rgb565_to_rgb24(src, dst, ctx->width * ctx->height);
    } else {
        memcpy(dst, src, ctx->frame_size);
    }
}

int camera_get_fd(const camera_ctx_t *ctx)           { return ctx ? ctx->fd : -1; }
int camera_get_width(const camera_ctx_t *ctx)        { return ctx ? ctx->width : 0; }
int camera_get_height(const camera_ctx_t *ctx)       { return ctx ? ctx->height : 0; }
int camera_get_pixelformat(const camera_ctx_t *ctx)  { return ctx ? ctx->pixelformat : 0; }
int camera_get_frame_size(const camera_ctx_t *ctx)   { return ctx ? ctx->frame_size : 0; }
int camera_get_bpp(const camera_ctx_t *ctx)          { return ctx ? ctx->bpp : 0; }
