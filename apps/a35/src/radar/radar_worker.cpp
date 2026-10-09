// Radar 线程独占串口与方向滤波器；输入字节流组帧后，只向 Main 发布结果快照。
#include "radar/radar_worker.hpp"
#include "runtime/linux_resources.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <thread>

void radar_receive_loop(const std::string &device,
                        helmet::BoundedQueue<RadarObservation> &observations) {
    try {
        while (g_running) {
            // 每次重连一个作用域；任何失败路径离开此作用域都会自动关闭旧串口。
            helmet::UniqueFd uart(open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC));
            if (uart.get() < 0 || set_uart(uart.get(), BAUDRATE) != 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            int setup = radar_init(uart.get());
            startup_mark(setup == 0 ? "radar_initialize_done" : "radar_initialize_unconfirmed");
            // 串口 read 不保留帧边界：pending 保存半帧，完整帧可能一次到达多份。
            uint8_t pending[512];
            size_t length = 0;
            uint64_t next_poll = helmet::monotonic_us();
            bool first_report = true;
            while (g_running) {
                auto now = helmet::monotonic_us();
                if (now >= next_poll) {
                    send_cmd(uart.get(), 1, 0x10, nullptr, 0);
                    next_poll = now + 2400000;
                }

                struct pollfd descriptor {
                    uart.get(), POLLIN, 0
                };

                int ready = poll(&descriptor, 1, 50);
                if ((ready < 0 && errno != EINTR) ||
                    descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
                    break;
                if (ready <= 0)
                    continue;
                if (length == sizeof(pending))
                    length = 0;
                ssize_t count = read(uart.get(), pending + length, sizeof(pending) - length);
                if (count < 0 && errno != EINTR && errno != EAGAIN)
                    break;
                if (count <= 0)
                    continue;
                length += static_cast<size_t>(count);
                // 无效帧头逐字节跳过，完整性不足时保留尾部，交给下一次 read 继续拼接。
                size_t consumed = 0;
                while (length - consumed >= 3) {
                    const uint8_t *frame = pending + consumed;
                    size_t total;
                    if (frame[0] == HEAD_REPORT)
                        total = 3 + frame[1];
                    else if (frame[0] == HEAD_REPLY)
                        total = 5 + frame[2];
                    else {
                        ++consumed;
                        continue;
                    }
                    if (total > sizeof(pending)) {
                        ++consumed;
                        continue;
                    }
                    if (length - consumed < total)
                        break;
                    RadarObservation observation;
                    if (process_radar_frame(frame, static_cast<int>(total), &observation.result) ==
                        1) {
                        observation.timestamp_us = helmet::monotonic_us();
                        // 主循环较慢时保留新报告，避免按旧位置/速度持续做风险判断。
                        observations.push(observation, helmet::OverflowPolicy::DropOldest);
                        if (first_report) {
                            startup_mark("radar_first_report");
                            first_report = false;
                        }
                    }
                    consumed += total;
                }
                memmove(pending, pending + consumed, length - consumed);
                length -= consumed;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    } catch (const std::exception &error) {
        fprintf(stderr, "[RADAR] Worker failed: %s\n", error.what());
    }
    observations.close();
}
