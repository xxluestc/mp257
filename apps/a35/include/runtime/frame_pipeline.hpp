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
inline uint64_t monotonic_us() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

struct Frame {
    std::vector<uint8_t> bytes;
    size_t size = 0;
    uint64_t timestamp_us = 0;
    uint64_t sequence = 0;
    int width = 0;
    int height = 0;
};

using FrameRef = std::shared_ptr<const Frame>;

// Only the producer can mutate a free slot. Published slots are immutable until
// the last reference disappears. The shared state also outlives the pool facade.
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
        // Establish the recycling guard before touching the leased slot.
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
        ready_.wait_for(lock, timeout, [this] { return closed_ || !values_.empty(); });
        if (values_.empty())
            return false;
        value = std::move(values_.front());
        values_.pop_front();
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        ready_.notify_all();
    }

    // Shutdown cancellation differs from close(): release queued ownership now.
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
        std::vector<FrameRef> result;
        for (const auto &frame : frames_)
            if (frame->timestamp_us >= begin && frame->timestamp_us <= end)
                result.push_back(frame);
        return result;
    }
};

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
        // Delayed or out-of-order notifications must not shorten an event.
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
        return value > std::numeric_limits<uint64_t>::max() - delta
                   ? std::numeric_limits<uint64_t>::max()
                   : value + delta;
    }
};
} // namespace helmet
