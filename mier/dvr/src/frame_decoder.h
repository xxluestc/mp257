/*
 * frame_decoder.h - 摄像头帧解码器 (MJPEG/YUYV → RGB)
 *
 * 用于NPU推理前将摄像头原始帧转换为RGB格式
 * 从 pro/camera_capture.c 中提取，适配DVR使用场景
 */

#ifndef FRAME_DECODER_H
#define FRAME_DECODER_H

#include <stdint.h>

/*
 * 将MJPEG帧解码为RGB
 * @param mjpeg_data   MJPEG数据
 * @param mjpeg_size   MJPEG数据大小
 * @param rgb_out      输出RGB缓冲区 (调用者分配, 大小 >= width*height*3)
 * @param width        期望宽度
 * @param height       期望高度
 * @return             0=成功, -1=失败
 */
int frame_decode_mjpeg_to_rgb(const uint8_t *mjpeg_data, int mjpeg_size,
                               uint8_t *rgb_out, int width, int height);

/*
 * 将YUYV帧转换为RGB
 * @param yuyv_data   YUYV数据
 * @param rgb_out     输出RGB缓冲区 (调用者分配, 大小 >= width*height*3)
 * @param width       宽度
 * @param height      高度
 */
void frame_decode_yuyv_to_rgb(const uint8_t *yuyv_data, uint8_t *rgb_out,
                               int width, int height);

#endif /* FRAME_DECODER_H */