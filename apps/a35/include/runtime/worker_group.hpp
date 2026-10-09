#pragma once

#include <atomic>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace helmet {
// Main 拥有的线程组：共享停止标志，析构时通知停止并 join 已经创建的线程。
// start/stop 仅由拥有者线程调用；被捕获的对象必须活到线程组退出之后。
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
        // 即使后续线程创建失败，之前启动的线程也能收到退出通知并被统一回收。
        running_ = 0;
        for (auto &thread : threads_)
            if (thread.joinable())
                thread.join();
        stopped_ = true;
    }
};
} // namespace helmet
