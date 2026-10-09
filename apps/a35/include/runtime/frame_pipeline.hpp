#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace helmet {
// 所有 A35 业务持续时间使用同一单调时钟；墙钟仅用于文件名和日志展示。
inline uint64_t monotonic_us() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

struct Frame {
    // bytes 是池槽预分配的容量，size 才是本帧有效 MJPEG 字节数。
    std::vector<uint8_t> bytes;
    size_t size = 0;
    uint64_t timestamp_us = 0; // Camera 取到帧时的 A35 单调时钟时间。
    uint64_t sequence = 0;
    int width = 0;
    int height = 0;
};

// NPU、ring 和编码队列共享同一帧；const 限定发布后的消费者不能修改图像。
using FrameRef = std::shared_ptr<const Frame>;

// 帧池预分配 JPEG 槽位。生产者只能写空闲槽，最后一个 FrameRef 释放后槽位才归还。
// State 被帧引用的删除器共同持有，FramePool 外壳析构也不会让在途帧悬空。
class FramePool {
    struct State {
        std::mutex mutex;
        std::vector<Frame> frames;
        std::vector<size_t> free;

        State(size_t count, size_t bytes) : frames(count) {
            free.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                frames[i].bytes.resize(bytes);
                free.push_back(i);
            }
        }
    };

    std::shared_ptr<State> state_;

  public:
    FramePool(size_t count, size_t bytes) {
        if (!count || !bytes)
            throw std::invalid_argument("empty frame pool");
        state_ = std::make_shared<State>(count, bytes);
    }

    FrameRef copy(const uint8_t *data, size_t size, uint64_t timestamp, uint64_t sequence,
                  int width, int height) {
        auto state = state_;
        size_t index;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!data || !size || state->free.empty() || size > state->frames[0].bytes.size())
                return {};
            index = state->free.back();
            state->free.pop_back();
        }
        // 先建立归还槽位的 RAII 引用，再复制数据；引用控制块分配失败也会归还槽位。
        std::shared_ptr<Frame> frame(&state->frames[index], [state, index](Frame *) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->free.push_back(index);
        });
        std::memcpy(frame->bytes.data(), data, size);
        frame->size = size;
        frame->timestamp_us = timestamp;
        frame->sequence = sequence;
        frame->width = width;
        frame->height = height;
        return frame;
    }

    size_t available() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->free.size();
    }
};

enum class OverflowPolicy { RejectNewest, DropOldest };

// 跨线程有界消息队列：锁保护队列状态，条件变量负责等待与关闭唤醒。
// 满载时由调用者选择拒绝新项或丢弃旧项，生产者不会等待消费者腾出容量。
template <typename T> class BoundedQueue {
    const size_t capacity_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<T> values_;
    bool closed_ = false;
    uint64_t dropped_ = 0;

  public:
    explicit BoundedQueue(size_t capacity) : capacity_(capacity) {
        if (!capacity)
            throw std::invalid_argument("zero queue capacity");
    }

    bool push(T value, OverflowPolicy policy = OverflowPolicy::RejectNewest) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_)
            return false;
        if (values_.size() == capacity_) {
            ++dropped_;
            if (policy == OverflowPolicy::RejectNewest)
                return false;
            values_.pop_front();
        }
        values_.push_back(std::move(value));
        ready_.notify_one();
        return true;
    }

    bool pop(T &value, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        // 带条件等待抵御虚假唤醒；关闭后仍可取走已有数据，空队列才返回 false。
        ready_.wait_for(lock, timeout, [this] { return closed_ || !values_.empty(); });
        if (values_.empty())
            return false;
        value = std::move(values_.front());
        values_.pop_front();
        return true;
    }

    void close() {
        // 正常收尾：拒绝新数据，允许消费者排空已有数据。
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        ready_.notify_all();
    }

    // 停机取消：立即释放排队对象；取消队列不会收回消费者已经取出的对象。
    void cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        values_.clear();
        ready_.notify_all();
    }

    bool drained() {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_ && values_.empty();
    }

    uint64_t dropped() {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }
};

// 只由 DVR 线程访问，因此无需额外加锁；同时按帧数和时间跨度限制 RAM 缓存。
class FrameRing {
    size_t capacity_;
    uint64_t window_us_;
    std::deque<FrameRef> frames_;

  public:
    FrameRing(size_t capacity, uint64_t window_us) : capacity_(capacity), window_us_(window_us) {
        if (!capacity || !window_us)
            throw std::invalid_argument("empty ring");
    }

    void expire(uint64_t now) {
        while (!frames_.empty() && now >= frames_.front()->timestamp_us &&
               now - frames_.front()->timestamp_us > window_us_)
            frames_.pop_front();
    }

    void append(FrameRef frame) {
        expire(frame->timestamp_us);
        if (frames_.size() == capacity_)
            frames_.pop_front();
        frames_.push_back(std::move(frame));
    }

    std::vector<FrameRef> snapshot(uint64_t trigger) const {
        return snapshot_between(trigger > window_us_ ? trigger - window_us_ : 0, trigger);
    }

    std::vector<FrameRef> snapshot_between(uint64_t begin, uint64_t end) const {
        // 快照复制的是共享引用，不复制 JPEG；ring 淘汰旧帧不影响在途录像持有的帧。
        std::vector<FrameRef> result;
        for (const auto &frame : frames_)
            if (frame->timestamp_us >= begin && frame->timestamp_us <= end)
                result.push_back(frame);
        return result;
    }
};

// 只管理录像时间边界，不管理文件或编码器。max_span 从首次触发起计算，限制后段。
class EventWindow {
    uint64_t first_ = 0;
    uint64_t end_ = 0;
    uint64_t post_;
    uint64_t max_span_;
    bool active_ = false;

  public:
    EventWindow(uint64_t post, uint64_t max_span) : post_(post), max_span_(max_span) {
        if (!post || max_span < post)
            throw std::invalid_argument("invalid event window");
    }

    void trigger(uint64_t now) {
        if (!active_) {
            first_ = now;
            active_ = true;
        }
        // 后续触发延长后段，但不超过首次触发的上限；延迟或乱序通知不能缩短窗口。
        end_ = std::max(end_, std::min(add(first_, max_span_), add(now, post_)));
    }

    uint64_t begin(uint64_t pre) const {
        return first_ > pre ? first_ - pre : 0;
    }

    uint64_t end() const {
        return end_;
    }

    bool contains(uint64_t timestamp, uint64_t pre) const {
        return active_ && timestamp >= begin(pre) && timestamp <= end_;
    }

    bool active() const {
        return active_;
    }

    bool due(uint64_t now) const {
        return active_ && now >= end_;
    }

    void finish() {
        active_ = false;
        first_ = end_ = 0;
    }

  private:
    static uint64_t add(uint64_t value, uint64_t delta) {
        // 使用饱和加法，时间戳加持续时间不能溢出并回绕为过去的时间。
        return value > std::numeric_limits<uint64_t>::max() - delta
                   ? std::numeric_limits<uint64_t>::max()
                   : value + delta;
    }
};
} // namespace helmet
