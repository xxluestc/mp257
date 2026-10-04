#include "jpeg_decode.h"
#include <stdio.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <string.h>

struct jpeg_failure {
    struct jpeg_error_mgr base;
    jmp_buf jump;
};

static void fail(j_common_ptr info) {
    struct jpeg_failure *error = (struct jpeg_failure *)info->err;
    longjmp(error->jump, 1);
}

static void silence(j_common_ptr info, int level) {
    (void)info;
    (void)level;
}

int jpeg_decode_rgb(const uint8_t *jpeg, size_t size, uint8_t *rgb, size_t capacity, int *width,
                    int *height) {
    struct jpeg_decompress_struct info;
    struct jpeg_failure error;
    memset(&info, 0, sizeof(info));
    info.err = jpeg_std_error(&error.base);
    error.base.error_exit = fail;
    error.base.emit_message = silence;
    if (setjmp(error.jump)) {
        jpeg_destroy_decompress(&info);
        return -1;
    }
    jpeg_create_decompress(&info);
    jpeg_mem_src(&info, jpeg, size);
    if (jpeg_read_header(&info, TRUE) != JPEG_HEADER_OK || !info.image_width ||
        !info.image_height || info.image_width > 4096 || info.image_height > 2160 ||
        (size_t)info.image_width * info.image_height > capacity / 3) {
        jpeg_destroy_decompress(&info);
        return -1;
    }
    info.out_color_space = JCS_RGB;
    jpeg_start_decompress(&info);
    if (info.output_components != 3) {
        jpeg_destroy_decompress(&info);
        return -1;
    }
    *width = (int)info.output_width;
    *height = (int)info.output_height;
    while (info.output_scanline < info.output_height) {
        uint8_t *row = rgb + (size_t)info.output_scanline * info.output_width * 3;
        jpeg_read_scanlines(&info, &row, 1);
    }
    jpeg_finish_decompress(&info);
    jpeg_destroy_decompress(&info);
    return 0;
}

void resize_rgb(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh) {
    for (int y = 0; y < dh; ++y)
        for (int x = 0; x < dw; ++x) {
            size_t source = ((size_t)(y * sh / dh) * sw + x * sw / dw) * 3;
            size_t destination = ((size_t)y * dw + x) * 3;
            memcpy(dst + destination, src + source, 3);
        }
}
