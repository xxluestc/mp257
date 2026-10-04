#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int jpeg_decode_rgb(const uint8_t *jpeg, size_t size, uint8_t *rgb, size_t capacity, int *width,
                    int *height);
void resize_rgb(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh);
#ifdef __cplusplus
}
#endif
