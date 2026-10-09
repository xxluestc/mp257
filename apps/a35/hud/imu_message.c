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
