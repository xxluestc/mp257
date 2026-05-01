#include "trigger_receiver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

struct trigger_ctx {
    int  fd;
    char path[256];
};

trigger_ctx_t *trigger_receiver_create(const char *pipe_path)
{
    trigger_ctx_t *ctx = calloc(1, sizeof(trigger_ctx_t));
    if (!ctx) return NULL;

    strncpy(ctx->path, pipe_path, sizeof(ctx->path) - 1);
    unlink(ctx->path);

    if (mkfifo(ctx->path, 0666) < 0) {
        fprintf(stderr, "[TRIGGER] mkfifo %s failed: %s\n", ctx->path, strerror(errno));
        free(ctx);
        return NULL;
    }

    ctx->fd = open(ctx->path, O_RDONLY | O_NONBLOCK);
    if (ctx->fd < 0) {
        fprintf(stderr, "[TRIGGER] open pipe failed: %s\n", strerror(errno));
        unlink(ctx->path);
        free(ctx);
        return NULL;
    }

    printf("[TRIGGER] Listening on %s\n", ctx->path);
    return ctx;
}

void trigger_receiver_destroy(trigger_ctx_t *ctx)
{
    if (!ctx) return;
    close(ctx->fd);
    unlink(ctx->path);
    free(ctx);
}

int trigger_receiver_get_fd(const trigger_ctx_t *ctx)
{
    return ctx ? ctx->fd : -1;
}

int trigger_receiver_process(trigger_ctx_t *ctx, trigger_callback_t cb, void *user_data)
{
    if (!ctx || !cb) return -1;

    char line[256];
    ssize_t n = read(ctx->fd, line, sizeof(line) - 1);
    if (n <= 0) return 0;

    line[n] = '\0';

    char *nl = strchr(line, '\n');
    if (nl) *nl = '\0';

    trigger_data_t data;
    memset(&data, 0, sizeof(data));
    data.timestamp = time(NULL);

    if (strncmp(line, "TARGET_ON", 9) == 0) {
        data.event = TRIGGER_TARGET_ON;
    } else if (strncmp(line, "TARGET_OFF", 10) == 0) {
        data.event = TRIGGER_TARGET_OFF;
    } else if (strncmp(line, "WARNING", 7) == 0) {
        data.event = TRIGGER_WARNING;
    } else if (strncmp(line, "FALL", 4) == 0) {
        data.event = TRIGGER_FALL;
    } else if (strncmp(line, "COLLISION", 9) == 0) {
        data.event = TRIGGER_COLLISION;
    } else {
        printf("[TRIGGER] Unknown command: %s\n", line);
        return 0;
    }

    printf("[TRIGGER] Received: %s\n", line);
    cb(&data, user_data);
    return 1;
}
