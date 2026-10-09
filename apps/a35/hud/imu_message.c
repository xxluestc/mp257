// 原位更新 JSON 元数据；对象与返回字符串有独立生命周期，见 imu_message.h 的约定。
#include "imu_message.h"

char *imu_message_to_json(cJSON *message, double hud_timestamp_ms) {
    if (!cJSON_IsObject(message))
        return NULL;
    cJSON_DeleteItemFromObjectCaseSensitive(message, "source");
    cJSON_DeleteItemFromObjectCaseSensitive(message, "hud_timestamp_ms");
    if (!cJSON_AddStringToObject(message, "source", "IMU") ||
        !cJSON_AddNumberToObject(message, "hud_timestamp_ms", hud_timestamp_ms))
        return NULL;
    return cJSON_PrintUnformatted(message);
}
