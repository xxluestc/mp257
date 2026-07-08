#include "camera.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <errno.h>

int camera_open(camera_t *cam, const char *device, int width, int height)
{
    memset(cam, 0, sizeof(*cam));
    cam->fd = -1;

    cam->fd = open(device, O_RDWR);
    if (cam->fd < 0) {
        perror("open camera");
        return -1;
    }

    /* query capabilities */
    struct v4l2_capability cap;
    if (ioctl(cam->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("VIDIOC_QUERYCAP");
        goto fail;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "Not a video capture device\n");
        goto fail;
    }
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "Device does not support streaming\n");
        goto fail;
    }

    /* set format - try MJPEG first */
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(cam->fd, VIDIOC_S_FMT, &fmt) < 0) {
        /* try YUYV */
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        if (ioctl(cam->fd, VIDIOC_S_FMT, &fmt) < 0) {
            perror("VIDIOC_S_FMT");
            goto fail;
        }
    }

    /* 设置帧率 25fps */
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = 25;
    if (ioctl(cam->fd, VIDIOC_S_PARM, &parm) < 0) {
        perror("VIDIOC_S_PARM");
    }

    cam->width = fmt.fmt.pix.width;
    cam->height = fmt.fmt.pix.height;
    cam->pixelformat = fmt.fmt.pix.pixelformat;
    cam->buf_size = fmt.fmt.pix.sizeimage;

    uint32_t fourcc = cam->pixelformat;
    const char *fmt_name = (cam->pixelformat == V4L2_PIX_FMT_MJPEG) ? "MJPEG" :
                           (cam->pixelformat == V4L2_PIX_FMT_YUYV) ? "YUYV" : "unknown";
    printf("Camera: %s, %dx%d, fmt=%s fourcc=%c%c%c%c, bytesperline=%u, sizeimage=%u, fps=%u/%u\n",
           device, cam->width, cam->height, fmt_name,
           (fourcc >> 0) & 0xFF, (fourcc >> 8) & 0xFF,
           (fourcc >> 16) & 0xFF, (fourcc >> 24) & 0xFF,
           fmt.fmt.pix.bytesperline, cam->buf_size,
           parm.parm.capture.timeperframe.denominator,
           parm.parm.capture.timeperframe.numerator);

    /* request buffers */
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(cam->fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS");
        goto fail;
    }
    cam->buf_count = req.count;

    cam->buffers = calloc(cam->buf_count, sizeof(void *));
    cam->buf_lengths = calloc(cam->buf_count, sizeof(unsigned int));

    for (int i = 0; i < cam->buf_count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(cam->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("VIDIOC_QUERYBUF");
            goto fail;
        }

        cam->buffers[i] = mmap(NULL, buf.length,
                               PROT_READ | PROT_WRITE, MAP_SHARED,
                               cam->fd, buf.m.offset);
        if (cam->buffers[i] == MAP_FAILED) {
            perror("mmap");
            goto fail;
        }
        cam->buf_lengths[i] = buf.length;
    }

    return 0;

fail:
    camera_close(cam);
    return -1;
}

int camera_start(camera_t *cam)
{
    for (int i = 0; i < cam->buf_count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF");
            return -1;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam->fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON");
        return -1;
    }

    return 0;
}

int camera_capture(camera_t *cam, uint8_t **out_buf, unsigned int *out_len)
{
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(cam->fd, VIDIOC_DQBUF, &buf) < 0) {
        perror("VIDIOC_DQBUF");
        return -1;
    }

    *out_buf = (uint8_t *)cam->buffers[buf.index];
    *out_len = buf.bytesused;

    /* re-queue */
    if (ioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
        perror("VIDIOC_QBUF");
        return -1;
    }

    return 0;
}

void camera_stop(camera_t *cam)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(cam->fd, VIDIOC_STREAMOFF, &type);
}

void camera_close(camera_t *cam)
{
    if (cam->fd >= 0) {
        for (int i = 0; i < cam->buf_count; i++) {
            if (cam->buffers[i] && cam->buffers[i] != MAP_FAILED) {
                munmap(cam->buffers[i], cam->buf_lengths[i]);
            }
        }
        close(cam->fd);
        cam->fd = -1;
    }
    free(cam->buffers);
    free(cam->buf_lengths);
    cam->buffers = NULL;
    cam->buf_lengths = NULL;
}