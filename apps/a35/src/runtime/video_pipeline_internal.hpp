#pragma once
#include "runtime/video_pipeline.hpp"

namespace helmet {
// 一次事件的传输通道：DVR 生产 frames，Encoder 消费；两者共享会话生命周期。
struct VideoPipeline::EncoderSession {
    BoundedQueue<FrameRef> frames;
    std::string output;
    std::atomic<bool> abort{false}; // 两个线程均可终止会话，失败录像不允许提交。
    uint64_t last_sequence = 0; // 仅 DVR 更新，防止 ring 快照与实时追加重复送帧。

    EncoderSession(size_t capacity, std::string path) : frames(capacity), output(std::move(path)) {}
};

}
