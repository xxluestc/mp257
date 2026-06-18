#ifndef TRIGGER_RECEIVER_H
#define TRIGGER_RECEIVER_H

#include "dvr_types.h"

typedef struct trigger_ctx trigger_ctx_t;

trigger_ctx_t *trigger_receiver_create(const char *pipe_path);
void           trigger_receiver_destroy(trigger_ctx_t *ctx);
int            trigger_receiver_get_fd(const trigger_ctx_t *ctx);
int            trigger_receiver_process(trigger_ctx_t *ctx,
                                        void (*callback)(const trigger_data_t *data, void *user),
                                        void *user_data);

#endif