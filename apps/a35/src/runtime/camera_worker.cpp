#include "runtime/video_pipeline.hpp"
#include "camera.h"
#include <cstdio>

namespace helmet {
namespace {
class CameraDevice {
  public:
    camera_t camera{};

    CameraDevice() {
        camera.fd = -1;
    }

    ~CameraDevice() {
        camera_close(&camera);
    }

    CameraDevice(const CameraDevice &) = delete;
    CameraDevice &operator=(const CameraDevice &) = delete;
};

class CameraLease {
    camera_t *camera_;

  public:
    explicit CameraLease(camera_t *camera) : camera_(camera) {}

    ~CameraLease() {
        camera_release(camera_);
    }

    int release() {
        return camera_release(camera_);
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
                // The negotiated camera rate may exceed 25 fps. Drain it, but
                // retain no more than the configured 375 frames per 15 seconds.
                if (timestamp < next_capture)
                    continue;
                next_capture = next_capture ? next_capture + 40000 : timestamp + 40000;
                if (next_capture < timestamp)
                    next_capture = timestamp + 40000;
                auto frame = pool_.copy(data, size, timestamp, ++sequence, device.camera.width,
                                        device.camera.height);
                if (lease.release() != 0)
                    break;
                if (!frame) {
                    ++capture_dropped_;
                } else {
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
            report("camera_failed", error.what());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

} // namespace helmet
