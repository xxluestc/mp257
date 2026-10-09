#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace helmet {
struct VideoConfig {
    std::string camera_device;
    std::string model_path;
    std::string labels_path;
    std::string output_directory;
    std::string mount_directory;
    std::string encoder_worker = "/xxl/camera_detect/scripts/dvr_encode_worker.py";
    float confidence = 0.60f;
    size_t pool_slots = 896;
    size_t max_jpeg_bytes = 256 * 1024;
    size_t pre_frames = 375;
    uint64_t pre_us = 15000000;
    uint64_t post_us = 15000000;
    uint64_t max_span_us = 60000000;
    unsigned inference_stride = 10;
};

// Pure validation runs before allocating the pool or opening any devices.
void validate_video_config(const VideoConfig &config);
bool valid_storage_paths(const std::string &mount, const std::string &output);
} // namespace helmet
