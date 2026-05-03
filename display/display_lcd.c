#include "display_lcd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

struct display_ctx {
    int            fd;
    int            width;
    int            height;
    int            screen_size;
    uint8_t       *fb_ptr;
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    int            src_bpp;
    int            src_pixelformat;
};

display_ctx_t *display_open(int width, int height)
{
    (void)width;
    (void)height;

    display_ctx_t *ctx = calloc(1, sizeof(display_ctx_t));
    if (!ctx) return NULL;

    ctx->fd = open("/dev/fb0", O_RDWR);
    if (ctx->fd < 0) {
        fprintf(stderr, "[DISPLAY] Cannot open /dev/fb0: %s\n", strerror(errno));
        free(ctx);
        return NULL;
    }

    if (ioctl(ctx->fd, FBIOGET_VSCREENINFO, &ctx->vinfo) < 0) {
        fprintf(stderr, "[DISPLAY] FBIOGET_VSCREENINFO failed: %s\n", strerror(errno));
        close(ctx->fd);
        free(ctx);
        return NULL;
    }

    if (ioctl(ctx->fd, FBIOGET_FSCREENINFO, &ctx->finfo) < 0) {
        fprintf(stderr, "[DISPLAY] FBIOGET_FSCREENINFO failed: %s\n", strerror(errno));
        close(ctx->fd);
        free(ctx);
        return NULL;
    }

    ctx->width       = ctx->vinfo.xres;
    ctx->height      = ctx->vinfo.yres;
    ctx->screen_size = ctx->finfo.smem_len;

    ctx->fb_ptr = mmap(NULL, ctx->screen_size, PROT_READ | PROT_WRITE,
                       MAP_SHARED, ctx->fd, 0);
    if (ctx->fb_ptr == MAP_FAILED) {
        fprintf(stderr, "[DISPLAY] mmap framebuffer failed: %s\n", strerror(errno));
        close(ctx->fd);
        free(ctx);
        return NULL;
    }

    printf("[DISPLAY] LCD: %dx%d, bpp=%d, size=%d bytes\n",
           ctx->width, ctx->height, ctx->vinfo.bits_per_pixel, ctx->screen_size);
    return ctx;
}

void display_close(display_ctx_t *ctx)
{
    if (!ctx) return;
    munmap(ctx->fb_ptr, ctx->screen_size);
    close(ctx->fd);
    free(ctx);
    printf("[DISPLAY] Closed\n");
}

void display_set_source_format(display_ctx_t *ctx, int pixelformat, int bpp)
{
    if (!ctx) return;
    ctx->src_pixelformat = pixelformat;
    ctx->src_bpp         = bpp;
}

int display_show_frame(display_ctx_t *ctx, const uint8_t *data, int width, int height)
{
    if (!ctx || !data) return -1;

    int dst_bpp   = ctx->vinfo.bits_per_pixel / 8;
    int stride    = (int)ctx->finfo.line_length;
    int disp_w    = ctx->width;
    int disp_h    = ctx->height;

    int copy_w = width  < disp_w ? width  : disp_w;
    int copy_h = height < disp_h ? height : disp_h;

    int off_x = (disp_w - copy_w) / 2;
    int off_y = (disp_h - copy_h) / 2;

    int src_bpp = ctx->src_bpp > 0 ? ctx->src_bpp : 3;
    int src_stride = width * src_bpp;

    if (dst_bpp == 2 && src_bpp == 2) {
        static uint16_t bright_tbl[32][64][32];
        static int tbl_init = 0;
        if (!tbl_init) {
            float brightness = 0.75f;
            for (int r = 0; r < 32; r++)
                for (int g = 0; g < 64; g++)
                    for (int b = 0; b < 32; b++) {
                        int rn = (int)(r * brightness); if (rn > 31) rn = 31;
                        int gn = (int)(g * brightness); if (gn > 63) gn = 63;
                        int bn = (int)(b * brightness); if (bn > 31) bn = 31;
                        bright_tbl[r][g][b] = (uint16_t)((rn << 11) | (gn << 5) | bn);
                    }
            tbl_init = 1;
        }
        for (int y = 0; y < copy_h; y++) {
            const uint8_t *src = data + (size_t)y * src_stride;
            uint8_t *dst = ctx->fb_ptr + (size_t)(off_y + y) * stride + (size_t)off_x * dst_bpp;
            const uint16_t *src16 = (const uint16_t *)src;
            uint16_t *dst16 = (uint16_t *)dst;
            for (int x = 0; x < copy_w; x++) {
                uint16_t v = src16[x];
                dst16[x] = bright_tbl[v >> 11][(v >> 5) & 0x3F][v & 0x1F];
            }
        }
    } else if (dst_bpp == 4 && src_bpp == 2) {
        for (int y = 0; y < copy_h; y++) {
            const uint8_t *src = data + (size_t)y * src_stride;
            uint8_t *dst = ctx->fb_ptr + (size_t)(off_y + y) * stride + (size_t)off_x * dst_bpp;
            for (int x = 0; x < copy_w; x++) {
                dst[x * 4]     = src[x * 2];
                dst[x * 4 + 1] = src[x * 2 + 1];
                dst[x * 4 + 2] = 0;
                dst[x * 4 + 3] = 0;
            }
        }
    } else if (dst_bpp == 2 && src_bpp == 3) {
        static uint8_t tbl_init = 0;
        static uint8_t r_tbl[256], g_tbl[256], b_tbl[256];
        if (!tbl_init) {
            for (int i = 0; i < 256; i++) {
                r_tbl[i] = (uint8_t)(i >> 3);
                g_tbl[i] = (uint8_t)(i >> 2);
                b_tbl[i] = (uint8_t)(i >> 3);
            }
            tbl_init = 1;
        }
        for (int y = 0; y < copy_h; y++) {
            const uint8_t *src_line = data + (size_t)y * src_stride;
            uint8_t *dst_line = ctx->fb_ptr + (size_t)(off_y + y) * stride + (size_t)off_x * dst_bpp;
            const uint8_t *src = src_line;
            uint8_t *dst = dst_line;
            for (int x = 0; x < copy_w; x++) {
                uint16_t v = ((uint16_t)r_tbl[src[0]] << 11) |
                             ((uint16_t)g_tbl[src[1]] << 5) |
                              (uint16_t)b_tbl[src[2]];
                dst[0] = (uint8_t)(v & 0xff);
                dst[1] = (uint8_t)(v >> 8);
                src += 3;
                dst += 2;
            }
        }
    } else if (dst_bpp == 4 && src_bpp == 3) {
        for (int y = 0; y < copy_h; y++) {
            const uint8_t *src = data + (size_t)y * src_stride;
            uint8_t *dst = ctx->fb_ptr + (size_t)(off_y + y) * stride + (size_t)off_x * dst_bpp;
            for (int x = 0; x < copy_w; x++) {
                dst[0] = src[2];
                dst[1] = src[1];
                dst[2] = src[0];
                dst[3] = 0;
                src += 3;
                dst += 4;
            }
        }
    } else {
        for (int y = 0; y < copy_h; y++) {
            const uint8_t *src = data + (size_t)y * src_stride;
            uint8_t *dst = ctx->fb_ptr + (size_t)(off_y + y) * stride + (size_t)off_x * dst_bpp;
            memcpy(dst, src, (size_t)(copy_w * src_bpp < copy_w * dst_bpp ? copy_w * src_bpp : copy_w * dst_bpp));
        }
    }

    return 0;
}

int display_get_fd(const display_ctx_t *ctx) { return ctx ? ctx->fd : -1; }
