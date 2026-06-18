#ifndef USB_CAMERA_H
#define USB_CAMERA_H

#include <stdint.h>

typedef struct usb_camera_ctx usb_camera_t;

usb_camera_t *usb_camera_open(const char *device, int width, int height, int fps);
void          usb_camera_close(usb_camera_t *ctx);
int           usb_camera_grab_frame(usb_camera_t *ctx, uint8_t *buffer, int buf_size, int64_t *ts_us);
int           usb_camera_get_fd(const usb_camera_t *ctx);
int           usb_camera_get_width(const usb_camera_t *ctx);
int           usb_camera_get_height(const usb_camera_t *ctx);
int           usb_camera_get_pixelformat(const usb_camera_t *ctx);
int           usb_camera_get_frame_size(const usb_camera_t *ctx);
const char   *usb_camera_get_pixelformat_name(const usb_camera_t *ctx);

#endif