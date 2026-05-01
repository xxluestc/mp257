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

int display_show_frame(display_ctx_t *ctx, const uint8_t *rgb24, int width, int height)
{
    if (!ctx || !rgb24) return -1;

    int bpp        = ctx->vinfo.bits_per_pixel / 8;
    int stride     = (int)ctx->finfo.line_length;
    int disp_w     = ctx->width;
    int disp_h     = ctx->height;

    int copy_w = width  < disp_w ? width  : disp_w;
    int copy_h = height < disp_h ? height : disp_h;

    int off_x = (disp_w - copy_w) / 2;
    int off_y = (disp_h - copy_h) / 2;

    for (int y = 0; y < copy_h; y++) {
        for (int x = 0; x < copy_w; x++) {
            int src_idx = (y * width + x) * 3;
            int dst_idx = ((off_y + y) * stride) + (off_x + x) * bpp;

            uint8_t r = rgb24[src_idx];
            uint8_t g = rgb24[src_idx + 1];
            uint8_t b = rgb24[src_idx + 2];

            if (bpp == 4) {
                ctx->fb_ptr[dst_idx]     = b;
                ctx->fb_ptr[dst_idx + 1] = g;
                ctx->fb_ptr[dst_idx + 2] = r;
                ctx->fb_ptr[dst_idx + 3] = 0;
            } else if (bpp == 2) {
                uint16_t rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
                ctx->fb_ptr[dst_idx]     = rgb565 & 0xff;
                ctx->fb_ptr[dst_idx + 1] = (rgb565 >> 8) & 0xff;
            }
        }
    }

    return 0;
}

int display_get_fd(const display_ctx_t *ctx) { return ctx ? ctx->fd : -1; }
