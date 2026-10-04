/**
 * camera.h - V4L2 摄像头采集接口
 *
 * 数据流:
 *   camera_open()  -> 打开设备，设置格式/帧率，申请 mmap 缓冲
 *   camera_start() -> 入队所有缓冲并开启视频流
 *   camera_capture() -> DQBUF 后借出当前帧，复制后 camera_release() 归还
 *   camera_stop()/camera_close() -> 停止视频流并释放资源
 *
 * 调用位置:
 *   - src/runtime/camera_worker.cpp (独占设备的采集线程)
 */
#ifndef CAMERA_H
#define CAMERA_H

#include <stdint.h>
#include <linux/videodev2.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief V4L2 摄像头实例
 * @note buffers[i] 为 mmap 映射后的内核缓冲用户态地址
 */
typedef struct {
    int fd;                    /* 设备文件描述符 */
    int width;                 /* 实际协商后的图像宽度 */
    int height;                /* 实际协商后的图像高度 */
    int pixelformat;           /* V4L2_PIX_FMT_MJPEG / YUYV 等 */
    unsigned int buf_size;     /* 每帧最大字节数 */
    int buf_count;             /* V4L2 缓冲数量 */
    void **buffers;            /* mmap'd 缓冲指针数组 */
    unsigned int *buf_lengths; /* 每个 mmap 缓冲的字节长度 */
    int acquired_index;        /* -1: no buffer leased to the caller */
    int streaming;
} camera_t;

/** 打开摄像头并设置采集参数 */
int camera_open(camera_t *cam, const char *device, int width, int height);

/** 启动视频流：入队缓冲 + STREAMON */
int camera_start(camera_t *cam);

/** Lease one buffer: 0 frame, 1 timeout/EAGAIN, -1 error. */
int camera_capture(camera_t *cam, uint8_t **out_buf, unsigned int *out_len);

/** Return the leased buffer after copying it. No pointer survives this call. */
int camera_release(camera_t *cam);

/** 停止视频流 (STREAMOFF) */
void camera_stop(camera_t *cam);

/** 关闭设备并释放 mmap 缓冲 */
void camera_close(camera_t *cam);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_H */
