#pragma once

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

// 原对象仍由调用者持有；更新来源和 HUD 时间后序列化，保留其他 IMU/GPS 字段。
// 返回文本由调用者用 cJSON_free() 释放，失败返回 NULL。
char *imu_message_to_json(cJSON *message, double hud_timestamp_ms);

#ifdef __cplusplus
}
#endif
