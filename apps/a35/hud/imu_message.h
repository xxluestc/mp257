#pragma once

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

// The caller retains the parsed object and releases the returned text with
// cJSON_free(). Existing IMU/GPS fields survive serialization.
char *imu_message_to_json(cJSON *message, double hud_timestamp_ms);

#ifdef __cplusplus
}
#endif
