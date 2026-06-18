#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include "dvr_types.h"
#include <pthread.h>
#include <sys/types.h>

/* 帧索引条目 */
typedef struct {
    int64_t timestamp_us;   /* 帧时间戳(微秒) */
    off_t   file_offset;    /* 在缓冲区文件中的字节偏移 */
    int     frame_size;     /* 实际帧数据大小(字节) */
} frame_index_t;

/* 待写入帧 */
typedef struct {
    uint8_t *data;
    int      size;
    int      capacity;
    int64_t  timestamp_us;
} pending_frame_t;

#define PENDING_QUEUE_SIZE 16
#define FRAME_HEADER_SIZE  12   /* uint32_t size + int64_t timestamp */

typedef struct {
    frame_index_t *index;        /* 帧索引数组 */
    int            capacity;     /* 最大帧数 */
    int            head;         /* 下一个写入位置 */
    int            tail;         /* 最旧有效帧位置 */
    int            count;        /* 当前有效帧数 */
    int            fd;           /* 缓冲区文件描述符 */
    char           filepath[256];
    off_t          file_size;    /* 文件当前大小 */
    off_t          write_pos;    /* 当前写入位置 */
    int            wrap_count;   /* 文件回绕次数 */

    /* 异步写线程 */
    pending_frame_t pending[PENDING_QUEUE_SIZE];
    int             pending_head;
    int             pending_tail;
    int             pending_count;
    pthread_mutex_t lock;
    pthread_cond_t  write_cond;
    pthread_t       write_thread;
    volatile int    write_thread_running;
    volatile int    paused;
    int             max_pending_size; /* pending帧最大数据大小 */
} ring_buffer_t;

ring_buffer_t *ring_buffer_create(int capacity_frames,
                                  const char *dir,
                                  int max_frame_size);
void           ring_buffer_destroy(ring_buffer_t *rb);
int            ring_buffer_push(ring_buffer_t *rb, const uint8_t *data,
                                int size, int64_t timestamp_us);
void           ring_buffer_clear(ring_buffer_t *rb);
void           ring_buffer_pause_writing(ring_buffer_t *rb);
void           ring_buffer_resume_writing(ring_buffer_t *rb);
int            ring_buffer_get_frame_count(const ring_buffer_t *rb);
int            ring_buffer_get_frame_size(const ring_buffer_t *rb, int idx);
int64_t        ring_buffer_get_frame_timestamp(const ring_buffer_t *rb, int idx);
off_t          ring_buffer_get_frame_offset(const ring_buffer_t *rb, int idx);

#endif