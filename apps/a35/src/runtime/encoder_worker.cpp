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
class ChildProcess {
    pid_t pid_ = -1;

  public:
    explicit ChildProcess(pid_t pid) : pid_(pid) {}

    ~ChildProcess() {
        if (pid_ > 0) {
            kill(-pid_, SIGKILL);
            while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
            }
        }
    }

    int finish(const std::atomic<bool> &stopping) {
        uint64_t deadline = monotonic_us() + 120000000;
        uint64_t stop_deadline = 0;
        while (monotonic_us() < deadline) {
            int status = 0;
            pid_t result = waitpid(pid_, &status, WNOHANG);
            if (result == pid_) {
                kill(-pid_, SIGKILL); // Reap any descendant left by a failed worker.
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

    ~SpawnAttributes() {
        posix_spawnattr_destroy(&value);
    }
};

bool write_bytes(int fd, const uint8_t *data, size_t size, const std::atomic<bool> &stopping) {
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
                // DVRSTREAM1 records are explicitly little endian, independent
                // of native struct padding and the target machine ABI.
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
            // A zero-size end record is emitted only for a complete event.
            // EOF alone (crash, overflow, shutdown) must never commit a partial clip.
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
        encoder_busy_ = false;
    }
}
} // namespace helmet
