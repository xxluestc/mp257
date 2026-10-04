#pragma once
#include "runtime/video_pipeline.hpp"

namespace helmet {
struct VideoPipeline::EncoderSession {
    BoundedQueue<FrameRef> frames;
    std::string output;
    std::atomic<bool> abort{false};
    uint64_t last_sequence = 0;

    EncoderSession(size_t capacity, std::string path) : frames(capacity), output(std::move(path)) {}
};

}
