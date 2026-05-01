#ifndef DVR_TYPES_H
#define DVR_TYPES_H

#include <stdint.h>
#include <time.h>

#define DVR_MAX_PATH        256
#define DVR_DEFAULT_FPS     30
#define DVR_DEFAULT_WIDTH   640
#define DVR_DEFAULT_HEIGHT  480
#define DVR_BUFFER_SECONDS  30
#define DVR_SAVE_BEFORE_SEC 15
#define DVR_SAVE_AFTER_SEC  15

typedef enum {
    DVR_STATE_IDLE       = 0,
    DVR_STATE_BUFFERING  = 1,
    DVR_STATE_SAVING     = 2,
} dvr_state_t;

typedef enum {
    TRIGGER_TARGET_ON      = 0,
    TRIGGER_TARGET_OFF     = 1,
    TRIGGER_WARNING        = 2,
    TRIGGER_FALL           = 3,
    TRIGGER_COLLISION      = 4,
} trigger_event_t;

typedef enum {
    DISPLAY_MODE_NONE = 0,
    DISPLAY_MODE_LCD  = 1,
    DISPLAY_MODE_QT   = 2,
} display_mode_t;

typedef struct {
    uint8_t *data;
    int      size;
    int      width;
    int      height;
    int      format;
    time_t   timestamp;
} frame_t;

typedef struct {
    trigger_event_t event;
    time_t          timestamp;
    int             object_id;
    char            extra[64];
} trigger_data_t;

typedef struct {
    int  buffer_seconds;
    int  save_before_seconds;
    int  save_after_seconds;
    int  fps;
    int  width;
    int  height;
    char sd_card_path[DVR_MAX_PATH];
    char camera_device[DVR_MAX_PATH];
    int  enable_display;
    display_mode_t display_mode;
} dvr_config_t;

#endif
