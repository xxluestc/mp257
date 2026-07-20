/**
 * jpeg_decoder.c - libjpeg JPEG 解码封装
 *
 * 数据流:
 *   jpeg_decode_rgb(jpeg_data, jpeg_size, out_rgb, &w, &h)
 *     -> jpeg_read_header -> jpeg_start_decompress -> 逐行读取 scanlines
 *
 * 调用位置:
 *   - main.cpp        (camera_detect)
 *   - fusion_main.cpp (fusion_detect)
 *   - radar_fusion.cpp (DVR/NPU 帧解码)
 *
 * 注意:
 *   - out_rgb 缓冲区必须预先分配 w*h*3 字节
 *   - 使用 setjmp/longjmp 处理 libjpeg 错误，避免调用 exit()
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>
#include <setjmp.h>

/**
 * @brief 自定义 libjpeg 错误管理器
 *
 * 通过 setjmp/longjmp 将 libjpeg 致命错误转换为函数返回 -1，
 * 避免默认行为直接调用 exit() 导致整个进程退出。
 */
struct my_error_mgr {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

/**
 * @brief libjpeg 错误退出回调
 * @param cinfo jpeg_decompress_struct 指针
 *
 * 发生致命错误时跳转回 jpeg_decode_rgb 的 setjmp 点，返回 -1。
 */
static void my_error_exit(j_common_ptr cinfo) {
    struct my_error_mgr *myerr = (struct my_error_mgr *)cinfo->err;
    (*cinfo->err->output_message)(cinfo);
    longjmp(myerr->setjmp_buffer, 1);
}

/**
 * Decode JPEG data to RGB24 buffer.
 * Returns 0 on success, -1 on error.
 * out_rgb must be pre-allocated to width * height * 3 bytes.
 */
int jpeg_decode_rgb(const unsigned char *jpeg_data, unsigned long jpeg_size,
                    unsigned char *out_rgb, int *out_width, int *out_height) {
    struct jpeg_decompress_struct cinfo;
    struct my_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = my_error_exit;

    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpeg_data, jpeg_size);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    *out_width = cinfo.output_width;
    *out_height = cinfo.output_height;

    int row_stride = cinfo.output_width * cinfo.output_components;
    unsigned char *row_ptr = out_rgb;

    while (cinfo.output_scanline < cinfo.output_height) {
        unsigned char *buffer_array[1];
        buffer_array[0] = row_ptr;
        jpeg_read_scanlines(&cinfo, buffer_array, 1);
        row_ptr += row_stride;
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return 0;
}