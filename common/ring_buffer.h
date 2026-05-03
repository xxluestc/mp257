#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include "dvr_types.h"
#include <pthread.h>
#include <sys/types.h>

typedef struct {
    time_t timestamp;
    off_t  offset;
} frame_index_t;

typedef struct {
    uint8_t *data;
    int      size;
    time_t   timestamp;
} pending_frame_t;

#define PENDING_QUEUE_SIZE 16

typedef struct {
    frame_index_t *index;
    int            capacity;
    int            head;
    int            tail;
    int            count;
    int            frame_size;
    int            fd;
    char           filepath[256];
    pthread_mutex_t lock;

    pending_frame_t pending[PENDING_QUEUE_SIZE];
    int             pending_head;
    int             pending_tail;
    int             pending_count;
    pthread_cond_t  write_cond;
    pthread_t       write_thread;
    volatile int    write_thread_running;
    volatile int    paused;
} ring_buffer_t;

ring_buffer_t *ring_buffer_create(int capacity_seconds, int fps,
                                  int width, int height, const char *dir,
                                  int frame_size);
void           ring_buffer_set_frame_size(ring_buffer_t *rb, int frame_size);
void           ring_buffer_destroy(ring_buffer_t *rb);
int            ring_buffer_push(ring_buffer_t *rb, const uint8_t *data,
                                int size, time_t ts);
int            ring_buffer_stream_range(ring_buffer_t *rb, time_t start,
                                        time_t end,
                                        int (*callback)(const frame_t *f, void *user),
                                        void *user);
int            ring_buffer_count(const ring_buffer_t *rb);
void           ring_buffer_clear(ring_buffer_t *rb);
void           ring_buffer_flush(ring_buffer_t *rb);
void           ring_buffer_pause_writing(ring_buffer_t *rb);
void           ring_buffer_resume_writing(ring_buffer_t *rb);

#endif
