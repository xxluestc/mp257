#include "ring_buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

ring_buffer_t *ring_buffer_create(int capacity_seconds, int fps,
                                  int width, int height, const char *dir)
{
    ring_buffer_t *rb = calloc(1, sizeof(ring_buffer_t));
    if (!rb) return NULL;

    rb->capacity   = capacity_seconds * fps;
    rb->frame_size = width * height * 3;
    rb->head       = 0;
    rb->tail       = 0;
    rb->count      = 0;

    snprintf(rb->filepath, sizeof(rb->filepath), "%s/dvr_buffer.bin", dir);

    rb->fd = open(rb->filepath, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (rb->fd < 0) {
        perror("[RINGBUF] open");
        free(rb);
        return NULL;
    }

    off_t total = (off_t)rb->capacity * rb->frame_size;
    if (ftruncate(rb->fd, total) < 0) {
        perror("[RINGBUF] ftruncate");
        close(rb->fd);
        free(rb);
        return NULL;
    }

    rb->index = calloc((size_t)rb->capacity, sizeof(frame_index_t));
    if (!rb->index) {
        close(rb->fd);
        free(rb);
        return NULL;
    }

    pthread_mutex_init(&rb->lock, NULL);
    printf("[RINGBUF] Single-file buffer: %s, %d frames, %d bytes/frame, %.1f MB total\n",
           rb->filepath, rb->capacity, rb->frame_size, (double)total / (1024 * 1024));
    return rb;
}

void ring_buffer_destroy(ring_buffer_t *rb)
{
    if (!rb) return;
    pthread_mutex_lock(&rb->lock);
    free(rb->index);
    if (rb->fd >= 0) close(rb->fd);
    pthread_mutex_unlock(&rb->lock);
    pthread_mutex_destroy(&rb->lock);
    unlink(rb->filepath);
    free(rb);
}

int ring_buffer_push(ring_buffer_t *rb, const uint8_t *data, int size, time_t ts)
{
    if (!rb || !data) return -1;

    pthread_mutex_lock(&rb->lock);

    off_t offset = (off_t)(rb->head % rb->capacity) * rb->frame_size;

    ssize_t n = pwrite(rb->fd, data, (size_t)(size < rb->frame_size ? size : rb->frame_size), offset);
    if (n < 0) {
        pthread_mutex_unlock(&rb->lock);
        return -1;
    }

    rb->index[rb->head % rb->capacity].timestamp = ts;
    rb->index[rb->head % rb->capacity].offset    = offset;

    rb->head++;
    if (rb->count < rb->capacity) {
        rb->count++;
    } else {
        rb->tail++;
    }

    pthread_mutex_unlock(&rb->lock);
    return 0;
}

int ring_buffer_stream_range(ring_buffer_t *rb, time_t start, time_t end,
                              int (*callback)(const frame_t *f, void *user),
                              void *user)
{
    if (!rb || !callback) return -1;

    pthread_mutex_lock(&rb->lock);

    int sent = 0;
    uint8_t *buf = malloc((size_t)rb->frame_size);
    if (!buf) {
        pthread_mutex_unlock(&rb->lock);
        return -1;
    }

    time_t first_ts = 0, last_ts = 0;

    for (int i = 0; i < rb->count; i++) {
        int pos = (rb->tail + i) % rb->capacity;

        if (i == 0) first_ts = rb->index[pos].timestamp;
        if (i == rb->count - 1) last_ts = rb->index[pos].timestamp;

        if (rb->index[pos].timestamp >= start &&
            rb->index[pos].timestamp <= end) {

            ssize_t n = pread(rb->fd, buf, (size_t)rb->frame_size, rb->index[pos].offset);
            if (n <= 0) continue;

            frame_t f;
            f.data      = buf;
            f.size      = (int)n;
            f.timestamp = rb->index[pos].timestamp;
            int ret = callback(&f, user);
            if (ret < 0) break;
            sent++;
        }
    }

    free(buf);
    pthread_mutex_unlock(&rb->lock);
    printf("[RINGBUF] stream_range: buf=[%ld,%ld] query=[%ld,%ld] count=%d sent=%d\n",
           (long)first_ts, (long)last_ts, (long)start, (long)end, rb->count, sent);
    return sent;
}

int ring_buffer_count(const ring_buffer_t *rb)
{
    if (!rb) return 0;
    int c;
    pthread_mutex_lock((pthread_mutex_t *)&rb->lock);
    c = rb->count;
    pthread_mutex_unlock((pthread_mutex_t *)&rb->lock);
    return c;
}

void ring_buffer_clear(ring_buffer_t *rb)
{
    if (!rb) return;
    pthread_mutex_lock(&rb->lock);
    rb->head  = 0;
    rb->tail  = 0;
    rb->count = 0;
    ftruncate(rb->fd, 0);
    ftruncate(rb->fd, (off_t)rb->capacity * rb->frame_size);
    pthread_mutex_unlock(&rb->lock);
}
