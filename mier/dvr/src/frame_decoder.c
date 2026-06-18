/*
 * frame_decoder.c - 摄像头帧解码器 (MJPEG/YUYV → RGB)
 *
 * 依赖: libjpeg (用于MJPEG解码)
 * 条件编译: 无 HAS_LIBJPEG 时 MJPEG 解码返回错误
 */

#include "frame_decoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#ifdef HAS_LIBJPEG
#include <jpeglib.h>
#endif

/* ===================================================================
 * YUYV → RGB
 * =================================================================== */
void frame_decode_yuyv_to_rgb(const uint8_t *yuyv, uint8_t *rgb,
                               int width, int height)
{
    int pixels = width * height;
    for (int i = 0; i < pixels / 2; i++) {
        int y0 = yuyv[i * 4 + 0];
        int u  = yuyv[i * 4 + 1];
        int y1 = yuyv[i * 4 + 2];
        int v  = yuyv[i * 4 + 3];

        int c = y0 - 16;
        int d = u - 128;
        int e = v - 128;

        int r0 = (298 * c + 409 * e + 128) >> 8;
        int g0 = (298 * c - 100 * d - 208 * e + 128) >> 8;
        int b0 = (298 * c + 516 * d + 128) >> 8;

        c = y1 - 16;
        int r1 = (298 * c + 409 * e + 128) >> 8;
        int g1 = (298 * c - 100 * d - 208 * e + 128) >> 8;
        int b1 = (298 * c + 516 * d + 128) >> 8;

        int idx = i * 6;
        rgb[idx + 0] = (uint8_t)(r0 < 0 ? 0 : (r0 > 255 ? 255 : r0));
        rgb[idx + 1] = (uint8_t)(g0 < 0 ? 0 : (g0 > 255 ? 255 : g0));
        rgb[idx + 2] = (uint8_t)(b0 < 0 ? 0 : (b0 > 255 ? 255 : b0));
        rgb[idx + 3] = (uint8_t)(r1 < 0 ? 0 : (r1 > 255 ? 255 : r1));
        rgb[idx + 4] = (uint8_t)(g1 < 0 ? 0 : (g1 > 255 ? 255 : g1));
        rgb[idx + 5] = (uint8_t)(b1 < 0 ? 0 : (b1 > 255 ? 255 : b1));
    }
}

/* ===================================================================
 * MJPEG → RGB (libjpeg)
 * =================================================================== */
#ifdef HAS_LIBJPEG

struct my_error_mgr {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

static void my_error_exit(j_common_ptr cinfo)
{
    struct my_error_mgr *err = (struct my_error_mgr *)cinfo->err;
    longjmp(err->setjmp_buffer, 1);
}

int frame_decode_mjpeg_to_rgb(const uint8_t *mjpeg_data, int mjpeg_size,
                               uint8_t *rgb_out, int width, int height)
{
    struct jpeg_decompress_struct cinfo;
    struct my_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = my_error_exit;

    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, mjpeg_data, (unsigned long)mjpeg_size);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    int row_stride = cinfo.output_width * cinfo.output_components;
    uint8_t *row_buf = rgb_out;

    while (cinfo.output_scanline < (unsigned int)cinfo.output_height) {
        uint8_t *row_ptr[1] = { row_buf };
        jpeg_read_scanlines(&cinfo, row_ptr, 1);
        row_buf += row_stride;
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return 0;
}

#else /* !HAS_LIBJPEG */

int frame_decode_mjpeg_to_rgb(const uint8_t *mjpeg_data, int mjpeg_size,
                               uint8_t *rgb_out, int width, int height)
{
    (void)mjpeg_data; (void)mjpeg_size; (void)rgb_out; (void)width; (void)height;
    fprintf(stderr, "[DECODE] MJPEG decode not available (no libjpeg)\n");
    return -1;
}

#endif /* HAS_LIBJPEG */