#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace helmet {
// 启动前生成、管线内保持不变的配置；时间单位为微秒，内存单位为字节。
struct VideoConfig {
    std::string camera_device;
    std::string model_path;
    std::string labels_path;
    std::string output_directory;
    std::string mount_directory;
    std::string encoder_worker = "/xxl/camera_detect/scripts/dvr_encode_worker.py";
    float confidence = 0.60f;
    size_t pool_slots = 896; // 默认 JPEG 负载预算：896 * 256 KiB = 224 MiB。
    size_t max_jpeg_bytes = 256 * 1024;
    size_t pre_frames = 375; // 25 fps 下容纳 15 秒；实际前段还受启动时长和掉帧影响。
    uint64_t pre_us = 15000000;
    uint64_t post_us = 15000000;
    uint64_t max_span_us = 60000000;
    unsigned inference_stride = 10; // 每十个保留帧送推理，录像仍接收所有保留帧。
};

// 纯校验先于帧池分配和设备打开，错误配置不能先触发大量内存分配。
void validate_video_config(const VideoConfig &config);
bool valid_storage_paths(const std::string &mount, const std::string &output);
} // namespace helmet
