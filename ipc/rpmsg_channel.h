#ifndef RPMSG_CHANNEL_H
#define RPMSG_CHANNEL_H

#include "dvr_types.h"

typedef struct rpmsg_ctx rpmsg_ctx_t;

typedef void (*rpmsg_callback_t)(const trigger_data_t *data, void *user_data);

rpmsg_ctx_t *rpmsg_channel_open(const char *tty_device);
void         rpmsg_channel_close(rpmsg_ctx_t *ctx);
int          rpmsg_channel_get_fd(const rpmsg_ctx_t *ctx);
int          rpmsg_channel_process(rpmsg_ctx_t *ctx, rpmsg_callback_t cb, void *user_data);

#endif
