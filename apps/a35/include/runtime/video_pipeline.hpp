#pragma once

#include "runtime/frame_pipeline.hpp"
#include "runtime/video_config.hpp"
#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace helmet {
struct NpuObservation {
    uint64_t timestamp_us = 0;
    bool has_road_user = false;
    unsigned road_count = 0;
    float score = 0;
    float inference_ms = 0;
    std::string label;
};

using VideoEvent = std::function<void(const char *event, const std::string &details)>;

class VideoPipeline {
    struct EncoderSession;
    VideoConfig config_;
    VideoEvent event_;
    FramePool pool_;
    BoundedQueue<FrameRef> npu_frames_{2};
    BoundedQueue<FrameRef> dvr_frames_{32};
    BoundedQueue<NpuObservation> observations_{8};
    BoundedQueue<uint64_t> triggers_{16};
    BoundedQueue<std::shared_ptr<EncoderSession>> sessions_{1};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> encoder_busy_{false};
    std::atomic<uint64_t> last_capture_{0};
    std::atomic<uint64_t> capture_dropped_{0};
    bool started_ = false;
    std::thread camera_thread_, npu_thread_, dvr_thread_, encoder_thread_;
    void capture_loop();
    void inference_loop();
    void dvr_loop();
    void encoder_loop();
    void report(const char *event, const std::string &details) noexcept;

  public:
    explicit VideoPipeline(VideoConfig config, VideoEvent event);
    ~VideoPipeline();
    VideoPipeline(const VideoPipeline &) = delete;
    VideoPipeline &operator=(const VideoPipeline &) = delete;
    // Single-use pipeline; lifecycle and trigger methods belong to Main/Fusion.
    void start();
    void stop();
    bool trigger(uint64_t timestamp_us);
    bool observation(NpuObservation &value);
    bool camera_available(uint64_t now) const;
};
} // namespace helmet
