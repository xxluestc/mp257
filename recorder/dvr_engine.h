#ifndef DVR_ENGINE_H
#define DVR_ENGINE_H

#include "dvr_types.h"
#include "ring_buffer.h"
#include "camera_v4l2.h"
#include "display_lcd.h"
#include "trigger_receiver.h"

typedef struct dvr_engine dvr_engine_t;

dvr_engine_t *dvr_engine_create(const dvr_config_t *config);
void          dvr_engine_destroy(dvr_engine_t *eng);
int           dvr_engine_run(dvr_engine_t *eng);
void          dvr_engine_stop(dvr_engine_t *eng);
dvr_state_t   dvr_engine_get_state(const dvr_engine_t *eng);

#endif
