#include "runtime/video_pipeline.hpp"
#include <cstdio>

namespace helmet {
VideoPipeline::VideoPipeline(VideoConfig config, VideoEvent event)
    : config_(std::move(config)), event_(std::move(event)),
      pool_(config_.pool_slots, config_.max_jpeg_bytes) {
    if (!config_.inference_stride || config_.pre_frames > 1500 ||
        config_.pool_slots < config_.pre_frames * 2 + 128)
        throw std::invalid_argument("video pool requires space for ring, encoder and workers");
}

VideoPipeline::~VideoPipeline() {
    stop();
}

void VideoPipeline::report(const char *event, const std::string &details) noexcept {
    try {
        event_(event, details);
    } catch (...) {
        fprintf(stderr, "[VIDEO] Event sink failed: %s\n", event);
    }
}

void VideoPipeline::start() {
    try {
        encoder_thread_ = std::thread(&VideoPipeline::encoder_loop, this);
        dvr_thread_ = std::thread(&VideoPipeline::dvr_loop, this);
        npu_thread_ = std::thread(&VideoPipeline::inference_loop, this);
        camera_thread_ = std::thread(&VideoPipeline::capture_loop, this);
    } catch (...) {
        stop();
        throw;
    }
}

void VideoPipeline::stop() {
    stopping_ = true;
    npu_frames_.close();
    dvr_frames_.close();
    triggers_.close();
    if (camera_thread_.joinable())
        camera_thread_.join();
    if (npu_thread_.joinable())
        npu_thread_.join();
    if (dvr_thread_.joinable())
        dvr_thread_.join();
    sessions_.close();
    if (encoder_thread_.joinable())
        encoder_thread_.join();
    observations_.close();
}

bool VideoPipeline::trigger(uint64_t timestamp) {
    return triggers_.push(timestamp);
}

bool VideoPipeline::observation(NpuObservation &value) {
    return observations_.pop(value, std::chrono::milliseconds(0));
}

bool VideoPipeline::camera_available(uint64_t now) const {
    auto last = last_capture_.load();
    return last && now >= last && now - last <= 1000000;
}

} // namespace helmet
