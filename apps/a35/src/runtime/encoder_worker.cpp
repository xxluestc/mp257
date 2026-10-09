// Encoder 线程：会话 JPEG -> 有界管道 -> Python 编码/校验进程 -> 保存结果事件。
#include "video_pipeline_internal.hpp"
#include "runtime/linux_resources.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

extern char **environ;

namespace helmet {
namespace {
// 独占工作进程及其进程组；异常离开作用域时终止整组，并 waitpid 回收直接子进程。
class ChildProcess {
    pid_t pid_ = -1;

  public:
    explicit ChildProcess(pid_t pid) : pid_(pid) {}

    ChildProcess(const ChildProcess &) = delete;
    ChildProcess &operator=(const ChildProcess &) = delete;

    ~ChildProcess() {
        if (pid_ > 0) {
            kill(-pid_, SIGKILL);
            while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
            }
        }
    }

    int finish(const std::atomic<bool> &stopping) {
        // 正常编码最长等待 120 秒；收到停机通知后给当前工作进程最多 2 秒收尾。
        uint64_t deadline = monotonic_us() + 120000000;
        uint64_t stop_deadline = 0;
        while (monotonic_us() < deadline) {
            int status = 0;
            pid_t result = waitpid(pid_, &status, WNOHANG);
            if (result == pid_) {
                kill(-pid_, SIGKILL); // 清除工作进程结束后仍留在组内的编码子进程。
                pid_ = -1;
                return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
            if (result < 0 && errno != EINTR)
                return -1;
            if (stopping && !stop_deadline)
                stop_deadline = monotonic_us() + 2000000;
            if (stop_deadline && monotonic_us() >= stop_deadline)
                return -1;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return -1;
    }
};

class SpawnActions {
  public:
    posix_spawn_file_actions_t value;

    SpawnActions() {
        if (posix_spawn_file_actions_init(&value))
            throw std::runtime_error("spawn actions init");
    }

    SpawnActions(const SpawnActions &) = delete;
    SpawnActions &operator=(const SpawnActions &) = delete;

    ~SpawnActions() {
        posix_spawn_file_actions_destroy(&value);
    }
};

class SpawnAttributes {
  public:
    posix_spawnattr_t value;

    SpawnAttributes() {
        if (posix_spawnattr_init(&value))
            throw std::runtime_error("spawn attributes init");
        if (posix_spawnattr_setpgroup(&value, 0) ||
            posix_spawnattr_setflags(&value, POSIX_SPAWN_SETPGROUP)) {
            posix_spawnattr_destroy(&value);
            throw std::runtime_error("spawn process group");
        }
    }

    SpawnAttributes(const SpawnAttributes &) = delete;
    SpawnAttributes &operator=(const SpawnAttributes &) = delete;

    ~SpawnAttributes() {
        posix_spawnattr_destroy(&value);
    }
};

bool write_bytes(int fd, const uint8_t *data, size_t size, const std::atomic<bool> &stopping) {
    // 管道允许短写；推进字节游标，并以 poll + 截止时间处理背压，避免无限阻塞。
    const uint64_t deadline = monotonic_us() + 3000000;
    while (size && !stopping && monotonic_us() < deadline) {
        ssize_t written = write(fd, data, size);
        if (written > 0) {
            data += written;
            size -= static_cast<size_t>(written);
        } else if (written < 0 && errno != EINTR && errno != EAGAIN) {
            return false;
        } else {
            struct pollfd descriptor {
                fd, POLLOUT, 0
            };

            if (poll(&descriptor, 1, 50) < 0 && errno != EINTR)
                return false;
            if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
                return false;
        }
    }
    return size == 0;
}

}

void VideoPipeline::encoder_loop() {
    std::shared_ptr<EncoderSession> session;
    while (!stopping_) {
        if (!sessions_.pop(session, std::chrono::milliseconds(100)))
            continue;
        bool ok = false;
        try {
            int descriptors[2];
            if (pipe2(descriptors, O_CLOEXEC))
                throw std::runtime_error("encoder pipe");
            UniqueFd reader(descriptors[0]), writer(descriptors[1]);
            SpawnActions actions;
            SpawnAttributes attributes;
            if (posix_spawn_file_actions_adddup2(&actions.value, reader.get(), STDIN_FILENO) ||
                posix_spawn_file_actions_addclose(&actions.value, writer.get()) ||
                (reader.get() != STDIN_FILENO &&
                 posix_spawn_file_actions_addclose(&actions.value, reader.get())))
                throw std::runtime_error("encoder spawn actions");
            // 使用参数数组启动，不经 shell 解释路径；工作进程通过 stdin 接收图像流。
            std::vector<std::string> arguments{
                "python3", config_.encoder_worker, "--stream", "--output", session->output,
                "--mount", config_.mount_directory};
            std::vector<char *> argv;
            for (auto &arg : arguments)
                argv.push_back(&arg[0]);
            argv.push_back(nullptr);
            pid_t pid;
            int result = posix_spawnp(&pid, "python3", &actions.value, &attributes.value,
                                      argv.data(), environ);
            if (result)
                throw std::runtime_error("encoder spawn: " + std::string(strerror(result)));
            ChildProcess child(pid);
            report("encoding_started", session->output);
            // 父进程仅保留写端；关闭写端才能让工作进程收到 EOF。
            reader.reset();
            if (fcntl(writer.get(), F_SETFL, O_NONBLOCK) < 0)
                throw std::runtime_error("encoder nonblocking pipe");
            FrameRef frame;
            bool transferred = true;
            while (!stopping_ && !session->abort) {
                if (!session->frames.pop(frame, std::chrono::milliseconds(100))) {
                    if (session->frames.drained())
                        break;
                    continue;
                }
                // 与 Python read_exact/records 对应：8 字节采集时间 + 4 字节 JPEG 长度，
                // 显式使用小端，避免直接传 C++ struct 的填充或目标机 ABI 差异。
                uint8_t header[12];
                for (int i = 0; i < 8; ++i)
                    header[i] = frame->timestamp_us >> (i * 8);
                for (int i = 0; i < 4; ++i)
                    header[8 + i] = frame->size >> (i * 8);
                transferred =
                    write_bytes(writer.get(), header, sizeof(header), stopping_) &&
                    write_bytes(writer.get(), frame->bytes.data(), frame->size, stopping_);
                frame.reset();
                if (!transferred)
                    break;
            }
            // 只有完整事件才发送零长度结束记录；单独 EOF 表示中断，不能提交半段视频。
            if (transferred && !session->abort && !stopping_) {
                uint8_t end[12]{};
                transferred = write_bytes(writer.get(), end, sizeof(end), stopping_);
            } else
                transferred = false;
            writer.reset();
            if (transferred)
                ok = child.finish(stopping_) == 0;
        } catch (const std::exception &error) {
            report("encoder_failed", error.what());
        }
        session->abort = true;
        session->frames.close();
        report(ok ? "recording_saved" : "recording_failed", session->output);
        session.reset();
        // 处理结果报告后才允许下一段录像；录像结果不改变 Fusion 当前风险状态。
        encoder_busy_ = false;
    }
}
} // namespace helmet
