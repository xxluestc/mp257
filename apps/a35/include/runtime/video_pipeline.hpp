#pragma once

#include "runtime/frame_pipeline.hpp"
#include <atomic>
#include <functional>
#include <string>
#include <thread>

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
    void start();
    void stop();
    bool trigger(uint64_t timestamp_us);
    bool observation(NpuObservation &value);
    bool camera_available(uint64_t now) const;
};
} // namespace helmet
