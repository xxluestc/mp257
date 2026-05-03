#include "ring_buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

static void *write_thread_func(void *arg);

ring_buffer_t *ring_buffer_create(int capacity_seconds, int fps,
                                  int width, int height, const char *dir,
                                  int frame_size)
{
    ring_buffer_t *rb = calloc(1, sizeof(ring_buffer_t));
    if (!rb) return NULL;

    rb->capacity   = capacity_seconds * fps;
    rb->frame_size = frame_size > 0 ? frame_size : width * height * 3;
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

    rb->index = calloc((size_t)rb->capacity, sizeof(frame_index_t));
    if (!rb->index) {
        close(rb->fd);
        free(rb);
        return NULL;
    }

    pthread_mutex_init(&rb->lock, NULL);
    pthread_cond_init(&rb->write_cond, NULL);

    for (int i = 0; i < PENDING_QUEUE_SIZE; i++) {
        rb->pending[i].data = malloc((size_t)rb->frame_size);
        if (!rb->pending[i].data) {
            for (int j = 0; j < i; j++) free(rb->pending[j].data);
            free(rb->index);
            close(rb->fd);
            free(rb);
            return NULL;
        }
        rb->pending[i].size = 0;
        rb->pending[i].timestamp = 0;
    }
    rb->pending_head = 0;
    rb->pending_tail = 0;
    rb->pending_count = 0;

    rb->write_thread_running = 1;
    if (pthread_create(&rb->write_thread, NULL, write_thread_func, rb) != 0) {
        perror("[RINGBUF] pthread_create");
        rb->write_thread_running = 0;
        for (int i = 0; i < PENDING_QUEUE_SIZE; i++) free(rb->pending[i].data);
        free(rb->index);
        close(rb->fd);
        free(rb);
        return NULL;
    }

    printf("[RINGBUF] Buffer: %s, %d frames, %d bytes/frame, %.1f MB max\n",
           rb->filepath, rb->capacity, rb->frame_size,
           (double)(rb->capacity * rb->frame_size) / (1024 * 1024));
    printf("[RINGBUF] Async write thread started (queue depth=%d)\n", PENDING_QUEUE_SIZE);
    return rb;
}

void ring_buffer_destroy(ring_buffer_t *rb)
{
    if (!rb) return;
    rb->write_thread_running = 0;
    pthread_cond_signal(&rb->write_cond);
    pthread_join(rb->write_thread, NULL);

    pthread_mutex_lock(&rb->lock);
    free(rb->index);
    for (int i = 0; i < PENDING_QUEUE_SIZE; i++) free(rb->pending[i].data);
    if (rb->fd >= 0) close(rb->fd);
    pthread_mutex_unlock(&rb->lock);

    pthread_mutex_destroy(&rb->lock);
    pthread_cond_destroy(&rb->write_cond);
    unlink(rb->filepath);
    free(rb);
}

static void *write_thread_func(void *arg)
{
    ring_buffer_t *rb = (ring_buffer_t *)arg;
    pending_frame_t local_pf;
    local_pf.data = malloc((size_t)rb->frame_size);
    if (!local_pf.data) return NULL;

    while (rb->write_thread_running) {
        pthread_mutex_lock(&rb->lock);

        while ((rb->pending_count == 0 || rb->paused) && rb->write_thread_running) {
            pthread_cond_wait(&rb->write_cond, &rb->lock);
        }

        if (!rb->write_thread_running) {
            pthread_mutex_unlock(&rb->lock);
            break;
        }

        local_pf = rb->pending[rb->pending_head];
        rb->pending_head = (rb->pending_head + 1) % PENDING_QUEUE_SIZE;
        rb->pending_count--;

        off_t offset = (off_t)(rb->head % rb->capacity) * rb->frame_size;
        time_t ts = local_pf.timestamp;
        int copy_size = local_pf.size;

        pthread_mutex_unlock(&rb->lock);

        ssize_t n = pwrite(rb->fd, local_pf.data, (size_t)copy_size, offset);

        pthread_mutex_lock(&rb->lock);
        if (n > 0) {
            rb->index[rb->head % rb->capacity].timestamp = ts;
            rb->index[rb->head % rb->capacity].offset    = offset;
            rb->head++;
            if (rb->count < rb->capacity) {
                rb->count++;
            } else {
                rb->tail++;
            }
        }
        pthread_mutex_unlock(&rb->lock);
    }

    free(local_pf.data);
    return NULL;
}

int ring_buffer_push(ring_buffer_t *rb, const uint8_t *data, int size, time_t ts)
{
    if (!rb || !data) return -1;

    pthread_mutex_lock(&rb->lock);

    if (rb->pending_count >= PENDING_QUEUE_SIZE) {
        rb->pending_tail = (rb->pending_tail + 1) % PENDING_QUEUE_SIZE;
        rb->pending_count--;
    }

    int slot = (rb->pending_tail + rb->pending_count) % PENDING_QUEUE_SIZE;
    int copy_size = size < rb->frame_size ? size : rb->frame_size;
    memcpy(rb->pending[slot].data, data, (size_t)copy_size);
    rb->pending[slot].size      = copy_size;
    rb->pending[slot].timestamp = ts;
    rb->pending_tail           = (rb->pending_tail + 1) % PENDING_QUEUE_SIZE;
    rb->pending_count++;

    pthread_cond_signal(&rb->write_cond);
    pthread_mutex_unlock(&rb->lock);

    return 0;
}

int ring_buffer_stream_range(ring_buffer_t *rb, time_t start, time_t end,
                              int (*callback)(const frame_t *f, void *user),
                              void *user_data)
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
            int ret = callback(&f, user_data);
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
    rb->pending_head  = 0;
    rb->pending_tail  = 0;
    rb->pending_count = 0;
    rb->head  = 0;
    rb->tail  = 0;
    rb->count = 0;
    pthread_mutex_unlock(&rb->lock);
}

void ring_buffer_flush(ring_buffer_t *rb)
{
    if (!rb) return;
    pthread_mutex_lock(&rb->lock);
    while (rb->pending_count > 0) {
        pthread_cond_signal(&rb->write_cond);
        pthread_mutex_unlock(&rb->lock);
        usleep(5000);
        pthread_mutex_lock(&rb->lock);
    }
    fsync(rb->fd);
    pthread_mutex_unlock(&rb->lock);
}

void ring_buffer_set_frame_size(ring_buffer_t *rb, int frame_size)
{
    if (!rb || frame_size <= 0) return;
    pthread_mutex_lock(&rb->lock);
    rb->frame_size = frame_size;
    for (int i = 0; i < PENDING_QUEUE_SIZE; i++) {
        free(rb->pending[i].data);
        rb->pending[i].data = malloc((size_t)frame_size);
    }
    printf("[RINGBUF] Frame size updated to %d bytes\n", frame_size);
    pthread_mutex_unlock(&rb->lock);
}

void ring_buffer_pause_writing(ring_buffer_t *rb)
{
    if (!rb) return;
    pthread_mutex_lock(&rb->lock);
    while (rb->pending_count > 0) {
        pthread_cond_signal(&rb->write_cond);
        pthread_mutex_unlock(&rb->lock);
        usleep(2000);
        pthread_mutex_lock(&rb->lock);
    }
    rb->paused = 1;
    fsync(rb->fd);
    pthread_mutex_unlock(&rb->lock);
}

void ring_buffer_resume_writing(ring_buffer_t *rb)
{
    if (!rb) return;
    pthread_mutex_lock(&rb->lock);
    rb->paused = 0;
    pthread_cond_signal(&rb->write_cond);
    pthread_mutex_unlock(&rb->lock);
}
