// NPU 线程：采样 JPEG -> RGB 解码/缩放 -> STAI 推理 -> 带采集时间的检测摘要。
#include "runtime/video_pipeline.hpp"
#include "npu_detect.h"
#include "vision/jpeg_decode.h"
#include <iterator>

namespace helmet {
namespace {
bool road_user(int index) {
    // COCO 道路使用者：人、自行车、汽车、摩托车、公交车、火车、卡车。
    const int classes[] = {1, 2, 3, 4, 6, 7, 8};
    return std::find(std::begin(classes), std::end(classes), index) != std::end(classes);
}
}

void VideoPipeline::inference_loop() {
    while (!stopping_) {
        try {
            // 模型与工作区由本线程独占；失败后离开作用域释放，再退避并重新加载。
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
                // 已排队过久的图像不再推理，避免耗费算力生产已经过期的风险依据。
                if (monotonic_us() - frame->timestamp_us > 1500000) {
                    frame.reset();
                    continue;
                }
                int width = 0, height = 0;
                uint64_t timestamp = frame->timestamp_us;
                int decoded = jpeg_decode_rgb(frame->bytes.data(), frame->size, rgb.data(),
                                              rgb.size(), &width, &height);
                frame.reset(); // 解码后不再需要 JPEG，推理期间槽位即可被 Camera 复用。
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
                // 发布检测摘要，不把 RGB 大缓冲传给 Main；视觉连续确认由 Fusion 完成。
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
