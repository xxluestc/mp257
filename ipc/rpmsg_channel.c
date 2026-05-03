#include "rpmsg_channel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>

struct rpmsg_ctx {
    int  fd;
    char device[256];
};

rpmsg_ctx_t *rpmsg_channel_open(const char *tty_device)
{
    rpmsg_ctx_t *ctx = calloc(1, sizeof(rpmsg_ctx_t));
    if (!ctx) return NULL;

    strncpy(ctx->device, tty_device, sizeof(ctx->device) - 1);

    ctx->fd = open(ctx->device, O_RDWR | O_NONBLOCK);
    if (ctx->fd < 0) {
        fprintf(stderr, "[RPMSG] Cannot open %s: %s\n", ctx->device, strerror(errno));
        free(ctx);
        return NULL;
    }

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(ctx->fd, &tty) == 0) {
        cfmakeraw(&tty);
        tty.c_cc[VMIN]  = 0;
        tty.c_cc[VTIME] = 0;
        tcsetattr(ctx->fd, TCSANOW, &tty);
    }

    printf("[RPMSG] Channel opened: %s (fd=%d)\n", ctx->device, ctx->fd);
    return ctx;
}

void rpmsg_channel_close(rpmsg_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->fd >= 0) close(ctx->fd);
    free(ctx);
    printf("[RPMSG] Channel closed\n");
}

int rpmsg_channel_get_fd(const rpmsg_ctx_t *ctx)
{
    return ctx ? ctx->fd : -1;
}

static int parse_and_dispatch(const char *cmd, rpmsg_callback_t cb, void *user_data)
{
    while (*cmd == ' ' || *cmd == '\t' || *cmd == '\r') cmd++;
    size_t len = strlen(cmd);
    while (len > 0 && (cmd[len-1] == ' ' || cmd[len-1] == '\t' || cmd[len-1] == '\r')) len--;
    if (len == 0) return 0;

    trigger_data_t data;
    memset(&data, 0, sizeof(data));
    data.timestamp = time(NULL);

    if (strncmp(cmd, "TARGET_ON", 9) == 0) {
        data.event = TRIGGER_TARGET_ON;
    } else if (strncmp(cmd, "TARGET_OFF", 10) == 0) {
        data.event = TRIGGER_TARGET_OFF;
    } else if (strncmp(cmd, "WARNING", 7) == 0) {
        data.event = TRIGGER_WARNING;
    } else if (strncmp(cmd, "FALL", 4) == 0) {
        data.event = TRIGGER_FALL;
    } else if (strncmp(cmd, "COLLISION", 9) == 0) {
        data.event = TRIGGER_COLLISION;
    } else {
        printf("[RPMSG] Unknown command: %.*s\n", (int)len, cmd);
        return 0;
    }

    printf("[RPMSG] Received from M-core: %.*s\n", (int)len, cmd);
    cb(&data, user_data);
    return 1;
}

int rpmsg_channel_process(rpmsg_ctx_t *ctx, rpmsg_callback_t cb, void *user_data)
{
    if (!ctx || !cb) return -1;

    char buf[1024];
    ssize_t n = read(ctx->fd, buf, sizeof(buf) - 1);
    if (n <= 0) return 0;

    buf[n] = '\0';
    int count = 0;
    char *p = buf;
    char *next;
    while ((next = strchr(p, '\n')) != NULL) {
        *next = '\0';
        count += parse_and_dispatch(p, cb, user_data);
        p = next + 1;
    }
    if (*p != '\0') {
        count += parse_and_dispatch(p, cb, user_data);
    }
    return count;
}
