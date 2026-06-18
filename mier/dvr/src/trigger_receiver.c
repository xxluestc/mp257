#include "trigger_receiver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

struct trigger_ctx {
    int   fd;
    char  path[256];
    char  buf[4096];
    int   buf_len;
};

trigger_ctx_t *trigger_receiver_create(const char *pipe_path)
{
    trigger_ctx_t *ctx = calloc(1, sizeof(trigger_ctx_t));
    if (!ctx) return NULL;

    strncpy(ctx->path, pipe_path, sizeof(ctx->path) - 1);

    /* 如果管道不存在则创建 */
    unlink(pipe_path);
    if (mkfifo(pipe_path, 0666) < 0) {
        fprintf(stderr, "[TRIGGER] mkfifo(%s) failed: %s\n", pipe_path, strerror(errno));
        free(ctx);
        return NULL;
    }

    /* 非阻塞打开管道 */
    ctx->fd = open(pipe_path, O_RDONLY | O_NONBLOCK);
    if (ctx->fd < 0) {
        fprintf(stderr, "[TRIGGER] open(%s) failed: %s\n", pipe_path, strerror(errno));
        unlink(pipe_path);
        free(ctx);
        return NULL;
    }

    printf("[TRIGGER] Pipe created: %s\n", pipe_path);
    return ctx;
}

void trigger_receiver_destroy(trigger_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->fd >= 0) close(ctx->fd);
    unlink(ctx->path);
    free(ctx);
}

int trigger_receiver_get_fd(const trigger_ctx_t *ctx)
{
    return ctx ? ctx->fd : -1;
}

static trigger_event_t parse_command(const char *cmd)
{
    if (strncmp(cmd, "TARGET_ON", 9) == 0)  return TRIGGER_TARGET_ON;
    if (strncmp(cmd, "TARGET_OFF", 10) == 0) return TRIGGER_TARGET_OFF;
    if (strncmp(cmd, "WARNING", 7) == 0)     return TRIGGER_WARNING;
    if (strncmp(cmd, "FALL", 4) == 0)        return TRIGGER_FALL;
    if (strncmp(cmd, "COLLISION", 9) == 0)   return TRIGGER_COLLISION;
    return -1;
}

static const char *event_name(trigger_event_t e)
{
    switch (e) {
    case TRIGGER_TARGET_ON:  return "TARGET_ON";
    case TRIGGER_TARGET_OFF: return "TARGET_OFF";
    case TRIGGER_WARNING:    return "WARNING";
    case TRIGGER_FALL:       return "FALL";
    case TRIGGER_COLLISION:  return "COLLISION";
    default: return "UNKNOWN";
    }
}

int trigger_receiver_process(trigger_ctx_t *ctx,
                              void (*callback)(const trigger_data_t *data, void *user),
                              void *user_data)
{
    if (!ctx || !callback) return 0;

    int remain = (int)sizeof(ctx->buf) - ctx->buf_len - 1;
    if (remain <= 0) {
        /* 缓冲区满, 丢弃旧数据 */
        ctx->buf_len = 0;
        remain = (int)sizeof(ctx->buf) - 1;
    }

    ssize_t n = read(ctx->fd, ctx->buf + ctx->buf_len, (size_t)remain);
    if (n <= 0) {
        if (n < 0 && errno != EAGAIN) {
            fprintf(stderr, "[TRIGGER] read error: %s\n", strerror(errno));
            return -1;
        }
        return 0;
    }

    ctx->buf_len += (int)n;
    ctx->buf[ctx->buf_len] = '\0';

    /* 按换行符分割处理命令 */
    char *saveptr;
    char *line = strtok_r(ctx->buf, "\n", &saveptr);
    int processed = 0;

    while (line) {
        /* 跳过空白 */
        while (*line == ' ' || *line == '\t' || *line == '\r') line++;

        trigger_event_t evt = parse_command(line);
        if (evt >= 0) {
            trigger_data_t data;
            memset(&data, 0, sizeof(data));
            data.event     = evt;
            data.timestamp = time(NULL);

            printf("[TRIGGER] Received: %s\n", event_name(evt));
            callback(&data, user_data);
            processed++;
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    /* 保留未完成的行 */
    if (saveptr && *saveptr) {
        int remaining = (int)strlen(saveptr);
        if (remaining > 0) {
            memmove(ctx->buf, saveptr, (size_t)remaining);
            ctx->buf_len = remaining;
        } else {
            ctx->buf_len = 0;
        }
    } else {
        ctx->buf_len = 0;
    }

    return processed;
}