#pragma once

#include <utility>
#include <unistd.h>

namespace helmet {
// 独占 fd 的 RAII 包装：禁止复制，移动后原对象变为空句柄，避免重复 close。
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
        // 转交给 fdopen 等接管句柄的接口；release 本身不关闭描述符。
        return std::exchange(fd_, -1);
    }

    int get() const noexcept {
        return fd_;
    }

    void reset(int fd = -1) noexcept {
        // close 不循环重试：描述符可能已被内核释放，再关闭可能误伤复用后的句柄。
        if (fd_ == fd)
            return;
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = fd;
    }
};

} // namespace helmet
