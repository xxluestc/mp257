#include "video_pipeline_internal.hpp"
#include <sys/stat.h>
#include <ctime>

namespace helmet {
namespace {
bool mounted(const std::string &path) {
    auto slash = path.find_last_of('/');
    std::string parent = slash == 0 ? "/" : path.substr(0, slash);

    struct stat mount_info {
    }, parent_info{};

    return stat(path.c_str(), &mount_info) == 0 && stat(parent.c_str(), &parent_info) == 0 &&
           S_ISDIR(mount_info.st_mode) && mount_info.st_dev != parent_info.st_dev;
}

std::string output_name(const std::string &directory, uint64_t event_id) {
    time_t now = time(nullptr);

    struct tm date {};

    localtime_r(&now, &date);
    char name[128];
    strftime(name, sizeof(name), "emergency_%Y%m%d_%H%M%S", &date);
    return directory + "/" + name + "_" + std::to_string(event_id) + ".mp4";
}

}

void VideoPipeline::dvr_loop() {
    std::shared_ptr<EncoderSession> current;
    try {
        FrameRing ring(config_.pre_frames, config_.pre_us);
        EventWindow window(config_.post_us, config_.max_span_us);
        FrameRef frame;
        while (!stopping_) {
            uint64_t now = monotonic_us();
            ring.expire(now);
            uint64_t trigger_time = 0;
            while (triggers_.pop(trigger_time, std::chrono::milliseconds(0))) {
                if (window.active()) {
                    window.trigger(now);
                    continue;
                }
                if (encoder_busy_ || !mounted(config_.mount_directory)) {
                    report("recording_rejected",
                           encoder_busy_ ? "encoder_busy" : "storage_unavailable");
                    continue;
                }
                current = std::make_shared<EncoderSession>(
                    config_.pre_frames + 64, output_name(config_.output_directory, trigger_time));
                auto before = ring.snapshot(now);
                for (auto &item : before) {
                    current->last_sequence = item->sequence;
                    current->frames.push(item);
                }
                encoder_busy_ = true;
                sessions_.push(current);
                window.trigger(now);
                report("recording_started",
                       "pre_frames=" + std::to_string(before.size()) + " path=" + current->output);
            }
            if (window.due(now) || (current && current->abort)) {
                current->frames.close();
                current.reset();
                window.finish();
            }
            if (!dvr_frames_.pop(frame, std::chrono::milliseconds(20)))
                continue;
            ring.append(frame);
            if (window.active() && frame->sequence > current->last_sequence) {
                current->last_sequence = frame->sequence;
                if (!current->frames.push(frame)) {
                    current->abort = true;
                    report("recording_overflow", current->output);
                }
            }
            frame.reset();
        }
        if (current) {
            current->abort = true;
            current->frames.close();
        }
    } catch (const std::exception &error) {
        if (current) {
            current->abort = true;
            current->frames.close();
        }
        report("dvr_failed", error.what());
    }
}

} // namespace helmet
