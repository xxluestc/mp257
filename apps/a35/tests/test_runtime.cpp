#include "runtime/linux_resources.hpp"
#include "runtime/video_config.hpp"
#include "runtime/worker_group.hpp"
#include "vision/ssd_postprocess.hpp"
#include "events/message_fields.hpp"
#include <atomic>
#include <cstdio>
#include <future>
#include <iostream>
#include <limits>

static void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

static void test_workers() {
    std::atomic<int> running{1};
    std::atomic<unsigned> finished{0};
    try {
        helmet::WorkerGroup workers(running);
        for (unsigned i = 0; i < 3; ++i)
            workers.start([&] {
                while (running)
                    std::this_thread::yield();
                ++finished;
            });
        throw std::runtime_error("simulated later startup failure");
    } catch (const std::runtime_error &) {
    }
    require(!running && finished == 3, "partial startup leaked a worker");
    helmet::WorkerGroup stopped(running);
    stopped.stop();
    stopped.stop();
    bool rejected = false;
    try {
        stopped.start([] {});
    } catch (const std::logic_error &) {
        rejected = true;
    }
    require(rejected, "stopped worker group restarted");
}

static void test_fd_ownership() {
    FILE *file = std::tmpfile();
    require(file != nullptr, "temporary file unavailable");
    const int descriptor = dup(fileno(file));
    require(descriptor >= 0, "descriptor duplication failed");
    {
        helmet::UniqueFd first(descriptor);
        helmet::UniqueFd moved(std::move(first));
        require(first.get() == -1 && moved.get() == descriptor, "move duplicated ownership");
        moved.reset(moved.get());
        require(write(moved.get(), "x", 1) == 1, "self-reset closed the active descriptor");
        helmet::UniqueFd assigned;
        assigned = std::move(moved);
        require(moved.get() == -1 && assigned.get() == descriptor,
                "move assignment lost ownership");
        require(assigned.release() == descriptor && assigned.get() == -1, "release closed the fd");
        first.reset(descriptor);
    }
    require(write(descriptor, "x", 1) == -1, "scope exit leaked the descriptor");
    std::fclose(file);
}

static void test_config() {
    helmet::VideoConfig valid;
    valid.camera_device = "/dev/video7";
    valid.model_path = "model.nb";
    valid.labels_path = "labels.txt";
    valid.mount_directory = "/run/media/tf";
    valid.output_directory = "/run/media/tf/recordings";
    helmet::validate_video_config(valid);
    auto rejected = [](const helmet::VideoConfig &config) {
        try {
            helmet::validate_video_config(config);
            return false;
        } catch (const std::invalid_argument &) {
            return true;
        }
    };
    auto bad = valid;
    bad.pool_slots = std::numeric_limits<size_t>::max();
    require(rejected(bad), "oversized pool was not rejected before allocation");
    bad = valid;
    bad.max_jpeg_bytes = 1024 * 1024 + 1;
    require(rejected(bad), "JPEG size exceeded the encoder protocol");
    bad = valid;
    bad.post_us = bad.max_span_us + 1;
    require(rejected(bad), "invalid post-event deadline accepted");
    bad = valid;
    bad.confidence = std::numeric_limits<float>::quiet_NaN();
    require(rejected(bad), "nonfinite confidence accepted");
    bad = valid;
    bad.camera_device.clear();
    require(rejected(bad), "empty device path accepted");
    for (const auto *path : {"/run/media/tf/..", "/run/media/tf/./video", "/run/media/tf2/video",
                             "/run/media/tf", "/run/media/tf//video"})
        require(!helmet::valid_storage_paths(valid.mount_directory, path),
                "output escaped the declared TF mount");
}

static void test_nms() {
    // A non-road class overlapping a person must not erase the road-user result.
    std::vector<float> boxes{0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1};
    auto detections = helmet::suppress_ssd_boxes(boxes, {17, 1, 1}, {0.99f, 0.9f, 0.8f}, 0.45f);
    require(detections.size() == 2 && detections[1].class_index == 1 && detections[1].score == 0.9f,
            "cross-class suppression lost a road user or retained a duplicate");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    detections = helmet::suppress_ssd_boxes(boxes, {1, 1, 1}, {nan, 0.9f, 0.8f}, 0.45f);
    require(detections.size() == 1 && detections[0].score == 0.9f, "invalid score reached NMS");
    boxes[4] = nan;
    boxes[10] = 0;
    require(helmet::suppress_ssd_boxes(boxes, {1, 1, 1}, {nan, 0.9f, 0.8f}, 0.45f).empty(),
            "invalid coordinates reached risk detection");
    bool rejected = false;
    try {
        helmet::suppress_ssd_boxes({0, 0, 1}, {1}, {0.9f}, 0.45f);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected, "malformed output lengths accepted");
}

static void test_rpmsg_messages() {
    constexpr auto line = "IMU_ALERT type=fall ax=12 max=99 seq=42 reason=tilt";
    require(helmet::message_integer(line, "ax") == 12 &&
                helmet::message_integer("IMU_ALERT max=99", "ax") == 0,
            "partial key matched a different IMU field");
    require(helmet::message_integer("seq=99999999999999999999", "seq") == 0 &&
                helmet::message_integer("seq=12garbage", "seq") == 0,
            "invalid integer reached event forwarding");
    require(helmet::message_field("IMU_ALERT type=falling", "type") != "fall",
            "partial type became a fall event");
    require(helmet::escape_json("a\"b\\c\n") == "a\\\"b\\\\c\\u000a",
            "reason field broke JSON escaping");
    helmet::MessageLine<8> lines;
    unsigned completed = 0;
    std::string accepted;
    for (const auto byte : std::string("oversized-event\nvalid\r\n"))
        if (const auto *message = lines.append(byte)) {
            ++completed;
            accepted = message;
        }
    require(completed == 1 && accepted == "valid", "oversized RPMsg line was forwarded in part");
    for (const auto byte : std::string("bad\0data\n", 9))
        require(lines.append(byte) == nullptr, "binary RPMsg line was accepted");
}

int main() {
    test_workers();
    test_fd_ownership();
    test_config();
    test_nms();
    test_rpmsg_messages();
    std::cout << "Worker/FD ownership, video configuration, NMS and RPMsg checks passed\n";
}
