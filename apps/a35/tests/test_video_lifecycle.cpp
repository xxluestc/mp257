#include "runtime/video_pipeline.hpp"
#include <iostream>
#include <limits>

namespace {
std::atomic<unsigned> entered{0};
helmet::FramePool *captured_pool = nullptr;

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
} // namespace

// Hardware workers are test doubles. Link the production VideoPipeline and
// config code, so these checks exercise its real start/stop/queue ownership.
namespace helmet {
void VideoPipeline::capture_loop() {
    captured_pool = &pool_;
    const uint8_t bytes[]{1, 2, 3, 4};
    auto frame = pool_.copy(bytes, sizeof(bytes), monotonic_us(), 1, 1, 1);
    npu_frames_.push(frame);
    dvr_frames_.push(frame);
    frame.reset();
    ++entered;
    while (!stopping_)
        std::this_thread::yield();
}

void VideoPipeline::inference_loop() {
    ++entered;
    while (!stopping_)
        std::this_thread::yield();
}

void VideoPipeline::dvr_loop() {
    inference_loop();
}

void VideoPipeline::encoder_loop() {
    inference_loop();
}
} // namespace helmet

int main() {
    helmet::VideoConfig config;
    config.camera_device = "fake_camera";
    config.model_path = "fake_model";
    config.labels_path = "fake_labels";
    config.mount_directory = "/tf";
    config.output_directory = "/tf/video";
    config.pre_frames = 1;
    config.pool_slots = 130;
    config.max_jpeg_bytes = 4;
    {
        helmet::VideoPipeline pipeline(config, {});
        require(!pipeline.trigger(1), "inactive pipeline accepted an event");
        pipeline.start();
        const auto deadline = helmet::monotonic_us() + 2000000;
        while (entered != 4 && helmet::monotonic_us() < deadline)
            std::this_thread::yield();
        require(entered == 4, "worker startup timed out");
        bool rejected = false;
        try {
            pipeline.start();
        } catch (const std::logic_error &) {
            rejected = true;
        }
        require(rejected, "duplicate start overwrote joinable threads");
        pipeline.stop();
        pipeline.stop();
        // stop() joins the writer and completes queue cancellation before
        // inspecting the pool. Seeing the stop flag alone is not that boundary.
        require(captured_pool && captured_pool->available() == config.pool_slots,
                "stop retained queued frame ownership");
        require(!pipeline.trigger(1), "stopped pipeline accepted an event");
        rejected = false;
        try {
            pipeline.start();
        } catch (const std::logic_error &) {
            rejected = true;
        }
        require(rejected, "closed queues were silently reused");
    }
    config.pool_slots = std::numeric_limits<size_t>::max();
    bool rejected = false;
    try {
        helmet::VideoPipeline invalid(config, {});
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected, "invalid configuration reached the allocation stage");
    std::cout << "Production pipeline start/stop and queued resource ownership checks passed\n";
}
