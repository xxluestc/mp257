// Camera 线程：V4L2 借帧 -> 复制到 RAM 池 -> 归还驱动缓冲 -> 分发 NPU/DVR 引用。
#include "runtime/video_pipeline.hpp"
#include "camera.h"
#include <cstdio>
#include <utility>

namespace helmet {
namespace {
// 封装 C 驱动接口的失败清理；打开、启动或后续采集失败时都沿同一析构路径关闭。
class CameraDevice {
  public:
    camera_t camera{};

    CameraDevice() {
        camera.fd = -1;
        camera.acquired_index = -1;
    }

    ~CameraDevice() {
        camera_close(&camera);
    }

    CameraDevice(const CameraDevice &) = delete;
    CameraDevice &operator=(const CameraDevice &) = delete;
};

// 一次 DQBUF 的借用凭证：提前 return/continue 或异常时也必须 QBUF。
class CameraLease {
    camera_t *camera_;

  public:
    explicit CameraLease(camera_t *camera) : camera_(camera) {}

    CameraLease(const CameraLease &) = delete;
    CameraLease &operator=(const CameraLease &) = delete;

    ~CameraLease() {
        if (camera_)
            camera_release(camera_);
    }

    int release() {
        // 先清空本地所有权，显式归还失败也不会在析构时重复归还同一个缓冲。
        return camera_ ? camera_release(std::exchange(camera_, nullptr)) : 0;
    }
};

}

void VideoPipeline::capture_loop() {
    uint64_t sequence = 0;
    uint64_t report_at = 0;
    while (!stopping_) {
        try {
            CameraDevice device;
            if (camera_open(&device.camera, config_.camera_device.c_str(), 1280, 720) ||
                camera_start(&device.camera)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            report("camera_attached", config_.camera_device);
            uint64_t next_capture = 0;
            while (!stopping_) {
                uint8_t *data = nullptr;
                unsigned int size = 0;
                int result = camera_capture(&device.camera, &data, &size);
                if (result > 0)
                    continue;
                if (result < 0)
                    break;
                CameraLease lease(&device.camera);
                uint64_t timestamp = monotonic_us();
                if (!last_capture_.load())
                    report("camera_first_frame", config_.camera_device);
                last_capture_ = timestamp;
                // 驱动实际帧率可能高于请求值；持续取帧并归还，但最多保留约 25 fps，
                // 让 375 槽 ring 对应约 15 秒。此处 continue 仍由 lease 自动 QBUF。
                if (timestamp < next_capture)
                    continue;
                next_capture = next_capture ? next_capture + 40000 : timestamp + 40000;
                if (next_capture < timestamp)
                    next_capture = timestamp + 40000;
                // 这是从驱动 MMAP 到应用 RAM 的一次拷贝；之后消费者只复制共享引用。
                auto frame = pool_.copy(data, size, timestamp, ++sequence, device.camera.width,
                                        device.camera.height);
                if (lease.release() != 0)
                    break;
                if (!frame) {
                    ++capture_dropped_;
                } else {
                    // 慢消费者不能扣住驱动缓冲或无限堆积；满载时用新帧替换排队旧帧。
                    dvr_frames_.push(frame, OverflowPolicy::DropOldest);
                    if (sequence % config_.inference_stride == 0)
                        npu_frames_.push(frame, OverflowPolicy::DropOldest);
                }
                if (timestamp >= report_at) {
                    report_at = timestamp + 5000000;
                    report("video_counters",
                           "capture_dropped=" + std::to_string(capture_dropped_) +
                               " npu_dropped=" + std::to_string(npu_frames_.dropped()) +
                               " dvr_dropped=" + std::to_string(dvr_frames_.dropped()) +
                               " pool_free=" + std::to_string(pool_.available()));
                }
            }
            last_capture_ = 0;
            report("camera_detached", config_.camera_device);
        } catch (const std::exception &error) {
            last_capture_ = 0;
            report("camera_failed", error.what());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

} // namespace helmet
