#pragma once

#include <atomic>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace helmet {
// Application-owned workers share one stop flag. Shutdown also handles a
// partially started group when allocation or thread creation throws.
// All lifecycle methods are called by the owning application thread.
class WorkerGroup {
    std::atomic<int> &running_;
    std::vector<std::thread> threads_;
    bool stopped_ = false;

  public:
    explicit WorkerGroup(std::atomic<int> &running) : running_(running) {}

    ~WorkerGroup() {
        stop();
    }

    WorkerGroup(const WorkerGroup &) = delete;
    WorkerGroup &operator=(const WorkerGroup &) = delete;

    template <typename F> void start(F &&function) {
        if (stopped_)
            throw std::logic_error("cannot restart a stopped worker group");
        threads_.emplace_back(std::forward<F>(function));
    }

    void stop() {
        if (stopped_)
            return;
        running_ = 0;
        for (auto &thread : threads_)
            if (thread.joinable())
                thread.join();
        stopped_ = true;
    }
};
} // namespace helmet
