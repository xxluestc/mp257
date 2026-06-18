#include "ring_buffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

static void *write_thread_func(void *arg);

ring_buffer_t *ring_buffer_create(int capacity_frames,
                                  const char *dir,
                                  int max_frame_size)
{
    ring_buffer_t *rb = calloc(1, sizeof(ring_buffer_t));
    if (!rb) return NULL;

    rb->capacity  = capacity_frames;
    rb->head      = 0;
    rb->tail      = 0;
    rb->count     = 0;
    rb->write_pos = 0;
    rb->file_size = (off_t)capacity_frames * (max_frame_size + FRAME_HEADER_SIZE);
    rb->max_pending_size = max_frame_size;

    snprintf(rb->filepath, sizeof(rb->filepath), "%s/dvr_buffer.bin", dir);

    rb->fd = open(rb->filepath, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (rb->fd < 0) {
        perror("[RINGBUF] open");
        free(rb);
        return NULL;
    }

    rb->index = calloc((size_t)capacity_frames, sizeof(frame_index_t));
    if (!rb->index) {
        close(rb->fd);
        free(rb);
        return NULL;
    }

    pthread_mutex_init(&rb->lock, NULL);
    pthread_cond_init(&rb->write_cond, NULL);

    for (int i = 0; i < PENDING_QUEUE_SIZE; i++) {
        rb->pending[i].data     = malloc((size_t)max_frame_size);
        rb->pending[i].capacity = max_frame_size;
        rb->pending[i].size     = 0;
        rb->pending[i].timestamp_us = 0;
        if (!rb->pending[i].data) {
            for (int j = 0; j < i; j++) free(rb->pending[j].data);
            free(rb->index);
            close(rb->fd);
            free(rb);
            return NULL;
        }
    }
    rb->pending_head  = 0;
    rb->pending_tail  = 0;
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

    printf("[RINGBUF] Buffer: %s, %d frames, ~%.1f MB max\n",
           rb->filepath, capacity_frames,
           (double)rb->file_size / (1024 * 1024));
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
    pending_frame_t local;
    local.data     = malloc((size_t)rb->max_pending_size);
    local.capacity = rb->max_pending_size;
    if (!local.data) return NULL;

    while (rb->write_thread_running) {
        pthread_mutex_lock(&rb->lock);

        while ((rb->pending_count == 0 || rb->paused) && rb->write_thread_running) {
            pthread_cond_wait(&rb->write_cond, &rb->lock);
        }

        if (!rb->write_thread_running) {
            pthread_mutex_unlock(&rb->lock);
            break;
        }

        /* 取出一个pending帧 */
        local.size         = rb->pending[rb->pending_head].size;
        local.timestamp_us = rb->pending[rb->pending_head].timestamp_us;
        memcpy(local.data, rb->pending[rb->pending_head].data, (size_t)local.size);
        rb->pending_head = (rb->pending_head + 1) % PENDING_QUEUE_SIZE;
        rb->pending_count--;

        /* 计算写入位置 */
        int entry_size = local.size + FRAME_HEADER_SIZE;

        pthread_mutex_unlock(&rb->lock);

        /* 在锁外执行磁盘写入 */
        /* 检查是否需要回绕 */
        if (rb->write_pos + entry_size > rb->file_size) {
            rb->write_pos  = 0;
            rb->wrap_count++;
        }

        /* 写入帧头: [uint32_t frame_size][int64_t timestamp] */
        uint8_t header[FRAME_HEADER_SIZE];
        uint32_t fsize = (uint32_t)local.size;
        memcpy(header, &fsize, 4);
        memcpy(header + 4, &local.timestamp_us, 8);

        ssize_t n1 = pwrite(rb->fd, header, FRAME_HEADER_SIZE, rb->write_pos);
        ssize_t n2 = pwrite(rb->fd, local.data, (size_t)local.size, rb->write_pos + FRAME_HEADER_SIZE);

        pthread_mutex_lock(&rb->lock);

        if (n1 == FRAME_HEADER_SIZE && n2 == local.size) {
            /* 写入成功, 更新索引 */
            int idx = rb->head;
            rb->index[idx].timestamp_us = local.timestamp_us;
            rb->index[idx].file_offset  = rb->write_pos;
            rb->index[idx].frame_size   = local.size;

            rb->write_pos += entry_size;
            rb->head = (rb->head + 1) % rb->capacity;

            if (rb->count < rb->capacity) {
                rb->count++;
            } else {
                /* 缓冲区满, 覆盖最旧帧 */
                rb->tail = (rb->tail + 1) % rb->capacity;
            }
        }

        pthread_mutex_unlock(&rb->lock);
    }

    free(local.data);
    return NULL;
}

int ring_buffer_push(ring_buffer_t *rb, const uint8_t *data, int size, int64_t timestamp_us)
{
    if (!rb || !data || size <= 0) return -1;

    pthread_mutex_lock(&rb->lock);

    if (rb->pending_count >= PENDING_QUEUE_SIZE) {
        /* 队列满, 丢弃最旧的pending帧 */
        rb->pending_tail = (rb->pending_tail + 1) % PENDING_QUEUE_SIZE;
        rb->pending_count--;
    }

    int slot = (rb->pending_head + rb->pending_count) % PENDING_QUEUE_SIZE;
    int copy_size = size < rb->pending[slot].capacity ? size : rb->pending[slot].capacity;
    memcpy(rb->pending[slot].data, data, (size_t)copy_size);
    rb->pending[slot].size         = copy_size;
    rb->pending[slot].timestamp_us = timestamp_us;

    rb->pending_count++;

    pthread_cond_signal(&rb->write_cond);
    pthread_mutex_unlock(&rb->lock);

    return 0;
}

void ring_buffer_clear(ring_buffer_t *rb)
{
    if (!rb) return;
    pthread_mutex_lock(&rb->lock);
    rb->pending_head  = 0;
    rb->pending_tail  = 0;
    rb->pending_count = 0;
    rb->head   = 0;
    rb->tail   = 0;
    rb->count  = 0;
    rb->write_pos = 0;
    rb->wrap_count = 0;
    /* 截断文件 */
    if (ftruncate(rb->fd, 0) < 0) { /* ignore error */ }
    pthread_mutex_unlock(&rb->lock);
}

void ring_buffer_pause_writing(ring_buffer_t *rb)
{
    if (!rb) return;
    pthread_mutex_lock(&rb->lock);
    /* 先清空pending队列 */
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

int ring_buffer_get_frame_count(const ring_buffer_t *rb)
{
    if (!rb) return 0;
    int c;
    pthread_mutex_lock((pthread_mutex_t *)&rb->lock);
    c = rb->count;
    pthread_mutex_unlock((pthread_mutex_t *)&rb->lock);
    return c;
}

int ring_buffer_get_frame_size(const ring_buffer_t *rb, int idx)
{
    if (!rb || idx < 0 || idx >= rb->count) return -1;
    int pos = (rb->tail + idx) % rb->capacity;
    return rb->index[pos].frame_size;
}

int64_t ring_buffer_get_frame_timestamp(const ring_buffer_t *rb, int idx)
{
    if (!rb || idx < 0 || idx >= rb->count) return -1;
    int pos = (rb->tail + idx) % rb->capacity;
    return rb->index[pos].timestamp_us;
}

off_t ring_buffer_get_frame_offset(const ring_buffer_t *rb, int idx)
{
    if (!rb || idx < 0 || idx >= rb->count) return -1;
    int pos = (rb->tail + idx) % rb->capacity;
    return rb->index[pos].file_offset;
}