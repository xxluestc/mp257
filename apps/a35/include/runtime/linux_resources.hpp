#pragma once

#include <utility>
#include <unistd.h>

namespace helmet {
class UniqueFd {
    int fd_ = -1;

  public:
    explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}

    ~UniqueFd() {
        reset();
    }

    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;

    UniqueFd(UniqueFd &&other) noexcept : fd_(other.release()) {}

    UniqueFd &operator=(UniqueFd &&other) noexcept {
        if (this != &other)
            reset(other.release());
        return *this;
    }

    int release() noexcept {
        return std::exchange(fd_, -1);
    }

    int get() const noexcept {
        return fd_;
    }

    void reset(int fd = -1) noexcept {
        if (fd_ == fd)
            return;
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = fd;
    }
};

} // namespace helmet
