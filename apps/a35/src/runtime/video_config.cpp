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
    return absolute_path(mount) && absolute_path(output) && output.size() > mount.size() &&
           output.compare(0, mount.size(), mount) == 0 && output[mount.size()] == '/';
}

void validate_video_config(const VideoConfig &config) {
    constexpr size_t max_pool_bytes = 512ULL * 1024 * 1024;
    // These limits also fit the Python encoder protocol's frame/duration bounds.
    if (!config.pre_frames || config.pre_frames > 375 || !config.pre_us ||
        config.pre_us > 15000000 || !config.post_us || config.max_span_us < config.post_us ||
        config.max_span_us > 60000000 || !config.inference_stride)
        throw std::invalid_argument("invalid video sampling or event window");
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
