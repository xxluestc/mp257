// 管线生命周期与对外入口；各工作线程的业务分别见 *_worker.cpp。
#include "runtime/video_pipeline.hpp"
#include <cstdio>

namespace helmet {
namespace {
VideoConfig checked_config(VideoConfig config) {
    validate_video_config(config);
    return config;
}
} // namespace

VideoPipeline::VideoPipeline(VideoConfig config, VideoEvent event)
    // 成员按声明顺序构造：先校验 config_，随后才按已校验的预算分配 pool_。
    : config_(checked_config(std::move(config))), event_(std::move(event)),
      pool_(config_.pool_slots, config_.max_jpeg_bytes) {}

VideoPipeline::~VideoPipeline() {
    stop();
}

void VideoPipeline::report(const char *event, const std::string &details) noexcept {
    // 日志/展示回调异常不能穿过线程入口，避免使整个进程 terminate。
    try {
        if (event_)
            event_(event, details);
    } catch (...) {
        fprintf(stderr, "[VIDEO] Event sink failed: %s\n", event);
    }
}

void VideoPipeline::start() {
    if (started_ || stopping_)
        throw std::logic_error("video pipeline is single-use");
    started_ = true;
    try {
        // 先启动消费者，最后启动 Camera 生产者；队列连接彼此，不共享设备句柄。
        encoder_thread_ = std::thread(&VideoPipeline::encoder_loop, this);
        dvr_thread_ = std::thread(&VideoPipeline::dvr_loop, this);
        npu_thread_ = std::thread(&VideoPipeline::inference_loop, this);
        camera_thread_ = std::thread(&VideoPipeline::capture_loop, this);
    } catch (...) {
        // 部分创建成功同样需要 join，不能让已启动线程访问构造后被销毁的对象。
        stop();
        throw;
    }
}

void VideoPipeline::stop() {
    // 先拒绝输入并唤醒等待者，再 join。看到 stopping_ 不代表资源清理已完成，
    // 只有本函数返回后，拥有者才能安全销毁队列、帧池和事件回调依赖。
    stopping_ = true;
    npu_frames_.cancel();
    dvr_frames_.cancel();
    triggers_.cancel();
    if (camera_thread_.joinable())
        camera_thread_.join();
    if (npu_thread_.joinable())
        npu_thread_.join();
    if (dvr_thread_.joinable())
        dvr_thread_.join();
    // DVR 已停止创建/续写会话，再取消待编码会话并等待当前编码工作退出。
    sessions_.cancel();
    if (encoder_thread_.joinable())
        encoder_thread_.join();
    observations_.cancel();
    encoder_busy_ = false;
}

bool VideoPipeline::trigger(uint64_t timestamp) {
    return started_ && !stopping_ && timestamp && triggers_.push(timestamp);
}

bool VideoPipeline::observation(NpuObservation &value) {
    return observations_.pop(value, std::chrono::milliseconds(0));
}

bool VideoPipeline::camera_available(uint64_t now) const {
    auto last = last_capture_.load();
    return last && now >= last && now - last <= 1000000;
}

} // namespace helmet
