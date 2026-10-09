#pragma once

#include "runtime/frame_pipeline.hpp"
#include "runtime/video_config.hpp"
#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace helmet {
struct NpuObservation {
    // 不携带图像所有权：Main 只需采集时间与检测摘要，JPEG 在推理线程提前释放。
    uint64_t timestamp_us = 0;
    bool has_road_user = false;
    unsigned road_count = 0;
    float score = 0;
    float inference_ms = 0;
    std::string label;
};

// 同步执行于报告事件的工作线程；回调必须允许并发，不能反向调用 start()/stop()。
using VideoEvent = std::function<void(const char *event, const std::string &details)>;

// 组合 Camera/NPU/DVR/Encoder 四个工作线程。设备和会话状态分别由对应线程持有，
// 跨线程帧通过共享引用和有界队列传递，Main 只消费观察结果、投递录像触发。
class VideoPipeline {
    struct EncoderSession;
    VideoConfig config_;
    VideoEvent event_;
    FramePool pool_; // 声明在队列之前，析构时队列先释放引用，帧池随后销毁。
    // Camera -> NPU/DVR；采集队列满时丢弃旧帧，优先保持实时性。
    BoundedQueue<FrameRef> npu_frames_{2};
    BoundedQueue<FrameRef> dvr_frames_{32};
    BoundedQueue<NpuObservation> observations_{8}; // NPU -> Main/Fusion。
    BoundedQueue<uint64_t> triggers_{16};
    BoundedQueue<std::shared_ptr<EncoderSession>> sessions_{1}; // DVR -> Encoder。
    std::atomic<bool> stopping_{false};
    std::atomic<bool> encoder_busy_{false};
    std::atomic<uint64_t> last_capture_{0};
    std::atomic<uint64_t> capture_dropped_{0};
    bool started_ = false; // 只在 Main 控制线程读写；stopping_ 供工作线程并发读取。
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
    // 单次启动；生命周期及触发接口由 Main/Fusion 调用，析构自动 stop 并 join。
    void start();
    void stop();
    bool trigger(uint64_t timestamp_us);
    bool observation(NpuObservation &value);
    bool camera_available(uint64_t now) const;
};
} // namespace helmet
