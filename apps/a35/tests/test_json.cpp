#include "imu_message.h"
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
using JsonText = std::unique_ptr<char, decltype(&cJSON_free)>;

static void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

int main() {
    const char *input = R"({"type":"fall_down","message":"a\"b\\c\n\u4e2d",
        "source":"M33_A35","requires_sms":true,"gps_valid":1,
        "lat_1e7":123456789,"lon_1e7":-234567890,"seq":42})";
    Json root(cJSON_ParseWithOpts(input, nullptr, 1), cJSON_Delete);
    require(root != nullptr, "valid escaped JSON was rejected");
    const auto *message = cJSON_GetObjectItem(root.get(), "message");
    require(cJSON_IsString(message) && std::strstr(message->valuestring, "a\"b\\c\n") != nullptr,
            "escape decoding changed navigation text");
    JsonText forwarded(imu_message_to_json(root.get(), 1234), cJSON_free);
    require(forwarded != nullptr, "IMU serialization failed");
    Json result(cJSON_ParseWithOpts(forwarded.get(), nullptr, 1), cJSON_Delete);
    require(result != nullptr, "forwarded JSON was malformed");
    require(std::strcmp(cJSON_GetObjectItem(result.get(), "message")->valuestring,
                        message->valuestring) == 0,
            "serialization changed escaped text");
    require(cJSON_GetObjectItem(result.get(), "lat_1e7")->valueint == 123456789 &&
                cJSON_GetObjectItem(result.get(), "lon_1e7")->valueint == -234567890 &&
                cJSON_IsTrue(cJSON_GetObjectItem(result.get(), "requires_sms")),
            "IMU forwarding lost GPS or SMS fields");
    require(std::strcmp(cJSON_GetObjectItem(result.get(), "source")->valuestring, "IMU") == 0 &&
                cJSON_GetObjectItem(result.get(), "hud_timestamp_ms")->valuedouble == 1234,
            "relay attribution is incorrect");
    for (const auto *invalid :
         {"{\"type\":}", "{\"a\":1,}", "{\"a\":1}trailer", "{\"text\":\"bad\\q\"}"}) {
        Json rejected(cJSON_ParseWithOpts(invalid, nullptr, 1), cJSON_Delete);
        require(!rejected, "malformed JSON accepted");
    }
    std::string nested(40, '[');
    nested += "0";
    nested += std::string(40, ']');
    Json rejected(cJSON_ParseWithOpts(nested.c_str(), nullptr, 1), cJSON_Delete);
    require(!rejected, "unbounded JSON nesting accepted");
    std::cout << "JSON escapes, malformed inputs, nesting and IMU/GPS forwarding checks passed\n";
}
