#ifndef DVR_TYPES_H
#define DVR_TYPES_H

#include <stdint.h>
#include <time.h>
#include <sys/time.h>

/* 微秒级时间戳 */
static inline int64_t dvr_time_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/* 配置常量 */
#define DVR_MAX_PATH          256
#define DVR_DEFAULT_FPS       25
#define DVR_DEFAULT_WIDTH     1280
#define DVR_DEFAULT_HEIGHT    720
#define DVR_BUFFER_SECONDS    38          /* 缓冲区时长(秒) */
#define DVR_SAVE_BEFORE_SEC   15          /* 触发前保存秒数 */
#define DVR_SAVE_AFTER_SEC    15          /* 触发后保存秒数 */
#define DVR_MAX_NORMAL_CLIPS  3           /* 普通片段最多保存个数 */

/* DVR 状态 */
typedef enum {
    DVR_STATE_IDLE      = 0,
    DVR_STATE_BUFFERING = 1,
    DVR_STATE_SAVING    = 2,
} dvr_state_t;

/* 触发事件类型 */
typedef enum {
    TRIGGER_TARGET_ON   = 0,
    TRIGGER_TARGET_OFF  = 1,
    TRIGGER_WARNING     = 2,
    TRIGGER_FALL        = 3,
    TRIGGER_COLLISION   = 4,
} trigger_event_t;

/* 片段类型 */
typedef enum {
    CLIP_TYPE_WARNING   = 0,
    CLIP_TYPE_FALL      = 1,
    CLIP_TYPE_COLLISION = 2,
} clip_type_t;

/* 触发数据 */
typedef struct {
    trigger_event_t event;
    time_t          timestamp;
    int             object_id;
    char            extra[64];
} trigger_data_t;

/* 自动触发事件类型 */
#define AUTO_EVENT_WARNING    0
#define AUTO_EVENT_COLLISION  1

/* NPU融合验证配置 */
#define NPU_CONFIRM_NEEDED  2   /* 需要连续确认N帧才信任道路用户 */
#define NPU_DENY_NEEDED     3   /* 连续否认N帧才判定为雷达误触发 */

/* DVR 配置 */
typedef struct {
    int  buffer_seconds;        /* 缓冲区时长(秒) */
    int  save_before_seconds;   /* 触发前保存秒数 */
    int  save_after_seconds;    /* 触发后保存秒数 */
    int  fps;
    int  width;
    int  height;
    char storage_path[DVR_MAX_PATH];    /* 存储路径 */
    char camera_device[DVR_MAX_PATH];   /* 摄像头设备 */

    /* 自动触发模式 (--auto) */
    int  auto_mode;             /* 0=手动管道模式, 1=自动触发模式 */
    int  auto_target_delay;     /* 启动后多少秒触发TARGET_ON (默认3) */
    int  auto_collision_delay;  /* TARGET_ON后多少秒触发碰撞 (默认25) */
    int  auto_event;            /* 自动触发的碰撞类型 (默认COLLISION) */

    /* NPU融合验证 (--npu-model) */
    char npu_model_path[DVR_MAX_PATH];   /* .nb模型文件路径 */
    char npu_labels_path[DVR_MAX_PATH];  /* 标签文件路径 */
    float npu_confidence;                /* 置信度阈值 (默认0.6) */
    int   npu_enabled;                   /* 是否启用NPU验证 */
} dvr_config_t;

#endif