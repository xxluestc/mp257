#pragma once

#include <thread>
#include <utility>
#include <unistd.h>

namespace helmet {
class UniqueFd {
    int fd_ = -1;

  public:
    explicit UniqueFd(int fd = -1) : fd_(fd) {}

    ~UniqueFd() {
        reset();
    }

    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;

    int get() const {
        return fd_;
    }

    void reset(int fd = -1) {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = fd;
    }
};

// The owner must request stop before destruction. Every successfully created
// thread is joined, including partial startup and exception paths.
class JoiningThread {
    std::thread thread_;

  public:
    template <typename F>
    explicit JoiningThread(F &&function) : thread_(std::forward<F>(function)) {}

    ~JoiningThread() {
        join();
    }

    JoiningThread(const JoiningThread &) = delete;
    JoiningThread &operator=(const JoiningThread &) = delete;

    void join() {
        if (thread_.joinable())
            thread_.join();
    }
};
} // namespace helmet
