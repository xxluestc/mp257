// 与设备无关的配置边界校验；命令行解析和 VideoPipeline 构造共用同一组约束。
#include "runtime/video_config.hpp"
#include <cmath>
#include <stdexcept>

namespace helmet {
namespace {
bool absolute_path(const std::string &path) {
    if (path.size() < 2 || path.front() != '/' || path.back() == '/')
        return false;
    size_t begin = 1;
    while (begin < path.size()) {
        size_t end = path.find('/', begin);
        auto component = path.substr(begin, end - begin);
        if (component.empty() || component == "." || component == "..")
            return false;
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return true;
}
} // namespace

bool valid_storage_paths(const std::string &mount, const std::string &output) {
    // 同时检查路径分量和目录边界，避免把 /tf_other 当作 /tf 的子目录。
    return absolute_path(mount) && absolute_path(output) && output.size() > mount.size() &&
           output.compare(0, mount.size(), mount) == 0 && output[mount.size()] == '/';
}

void validate_video_config(const VideoConfig &config) {
    constexpr size_t max_pool_bytes = 512ULL * 1024 * 1024;
    // 时间、帧数和 JPEG 大小同时受 Python 编码流协议约束，修改时需同步两端。
    if (!config.pre_frames || config.pre_frames > 375 || !config.pre_us ||
        config.pre_us > 15000000 || !config.post_us || config.max_span_us < config.post_us ||
        config.max_span_us > 60000000 || !config.inference_stride)
        throw std::invalid_argument("invalid video sampling or event window");
    // ring、正在传输的前段及队列都可能持有帧；预算留出两份前段与固定余量。
    // 用除法比较总容量，避免 pool_slots * max_jpeg_bytes 先乘法溢出。
    if (!config.max_jpeg_bytes || config.max_jpeg_bytes > 1024 * 1024 ||
        config.pool_slots < config.pre_frames * 2 + 128 ||
        config.pool_slots > max_pool_bytes / config.max_jpeg_bytes)
        throw std::invalid_argument("video pool needs ring/encoder headroom within 512 MiB");
    if (!std::isfinite(config.confidence) || config.confidence <= 0 || config.confidence > 1)
        throw std::invalid_argument("confidence must be finite and in (0, 1]");
    if (config.camera_device.empty() || config.model_path.empty() || config.labels_path.empty() ||
        config.encoder_worker.empty())
        throw std::invalid_argument("video device, model, labels and encoder paths are required");
    if (!valid_storage_paths(config.mount_directory, config.output_directory))
        throw std::invalid_argument("video output must be an absolute child of its mount");
}
} // namespace helmet
