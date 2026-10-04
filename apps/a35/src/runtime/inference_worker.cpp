#include "runtime/video_pipeline.hpp"
#include "npu_detect.h"
#include "vision/jpeg_decode.h"
#include <iterator>

namespace helmet {
namespace {
bool road_user(int index) {
    const int classes[] = {1, 2, 3, 4, 6, 7, 8};
    return std::find(std::begin(classes), std::end(classes), index) != std::end(classes);
}
}

void VideoPipeline::inference_loop() {
    while (!stopping_) {
        try {
            NpuDetector detector(config_.model_path.c_str(), config_.labels_path.c_str(),
                                 config_.confidence, 0.45f);
            int nw = detector.get_input_width(), nh = detector.get_input_height();
            std::vector<uint8_t> rgb(4096ULL * 2160 * 3);
            std::vector<uint8_t> input(static_cast<size_t>(nw) * nh * 3);
            report("npu_attached", config_.model_path);
            FrameRef frame;
            while (!stopping_) {
                if (!npu_frames_.pop(frame, std::chrono::milliseconds(100)))
                    continue;
                if (monotonic_us() - frame->timestamp_us > 1500000) {
                    frame.reset();
                    continue;
                }
                int width = 0, height = 0;
                uint64_t timestamp = frame->timestamp_us;
                int decoded = jpeg_decode_rgb(frame->bytes.data(), frame->size, rgb.data(),
                                              rgb.size(), &width, &height);
                frame.reset(); // JPEG slot is reusable throughout inference.
                if (decoded) {
                    report("jpeg_failed", "invalid or oversized JPEG");
                    continue;
                }
                resize_rgb(rgb.data(), width, height, input.data(), nw, nh);
                auto results = detector.detect(input.data());
                NpuObservation observation;
                observation.timestamp_us = timestamp;
                observation.inference_ms = results.inference_time_ms;
                for (const auto &object : results.objects) {
                    if (!road_user(object.class_index))
                        continue;
                    observation.has_road_user = true;
                    ++observation.road_count;
                    if (object.score > observation.score) {
                        observation.score = object.score;
                        observation.label = detector.get_label(object.class_index);
                    }
                }
                observations_.push(std::move(observation), OverflowPolicy::DropOldest);
            }
        } catch (const std::exception &error) {
            report("npu_failed", error.what());
        }
        for (int i = 0; i < 10 && !stopping_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

} // namespace helmet
