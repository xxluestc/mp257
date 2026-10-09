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
        auto finish_session = [&] {
            if (current)
                current->frames.close();
            current.reset();
            window.finish();
        };
        auto append_to_session = [&](const FrameRef &item) {
            if (!current || current->abort || item->sequence <= current->last_sequence ||
                !window.contains(item->timestamp_us, config_.pre_us))
                return;
            current->last_sequence = item->sequence;
            if (!current->frames.push(item)) {
                current->abort = true;
                report("recording_overflow", current->output);
            }
        };
        FrameRef frame;
        while (!stopping_) {
            // Drain one bounded batch before triggers, so a queued frame from
            // just before the event participates in its pre-event snapshot.
            for (unsigned count = 0; count < 32; ++count) {
                if (!dvr_frames_.pop(frame, std::chrono::milliseconds(count ? 0 : 20)))
                    break;
                ring.append(frame);
                append_to_session(frame);
                frame.reset();
            }
            uint64_t now = monotonic_us();
            ring.expire(now);
            if (current && current->abort)
                finish_session();
            uint64_t trigger_time = 0;
            while (triggers_.pop(trigger_time, std::chrono::milliseconds(0))) {
                now = monotonic_us();
                if (trigger_time > now || !trigger_time || now - trigger_time >= config_.post_us) {
                    report("recording_rejected", "stale_or_future_trigger");
                    continue;
                }
                // A trigger after the old deadline starts a separate event;
                // it cannot revive an already ended or duration-capped clip.
                if (window.active() && trigger_time >= window.end())
                    finish_session();
                if (window.active()) {
                    window.trigger(trigger_time);
                    // Restore frames that preceded processing of an extension
                    // but fell just beyond the previous end timestamp.
                    for (const auto &item :
                         ring.snapshot_between(window.begin(config_.pre_us), window.end()))
                        append_to_session(item);
                    continue;
                }
                if (encoder_busy_ || !mounted(config_.mount_directory)) {
                    report("recording_rejected",
                           encoder_busy_ ? "encoder_busy" : "storage_unavailable");
                    continue;
                }
                current = std::make_shared<EncoderSession>(
                    config_.pre_frames + 64, output_name(config_.output_directory, trigger_time));
                window.trigger(trigger_time);
                auto buffered = ring.snapshot_between(window.begin(config_.pre_us), window.end());
                size_t pre_frames = 0;
                for (const auto &item : buffered) {
                    if (item->timestamp_us <= trigger_time)
                        ++pre_frames;
                    append_to_session(item);
                }
                encoder_busy_ = true;
                if (!sessions_.push(current)) {
                    current->abort = true;
                    finish_session();
                    encoder_busy_ = false;
                    report("recording_rejected", "encoder_queue_closed");
                    continue;
                }
                report("recording_started",
                       "pre_frames=" + std::to_string(pre_frames) + " path=" + current->output);
            }
            if (window.due(now) || (current && current->abort))
                finish_session();
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
