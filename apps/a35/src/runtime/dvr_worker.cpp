// DVR 线程只维护 RAM ring、事件窗口和帧投递；写视频文件由独立 Encoder 链路完成。
#include "video_pipeline_internal.hpp"
#include <sys/stat.h>
#include <ctime>

namespace helmet {
namespace {
bool mounted(const std::string &path) {
    // 与父目录设备号比较，区分真实挂载和同名空目录，避免 TF 缺失时写进 rootfs。
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
            // 正常结束仅关闭输入，Encoder 仍持有共享会话并继续排空已接收的帧。
            // 此处不清 encoder_busy_；最终提交/失败由 Encoder 负责报告并解除占用。
            if (current)
                current->frames.close();
            current.reset();
            window.finish();
        };
        auto append_to_session = [&](const FrameRef &item) {
            // ring 快照与实时帧可能重叠，按采集序号去重，再按事件时间边界筛选。
            if (!current || current->abort || item->sequence <= current->last_sequence ||
                !window.contains(item->timestamp_us, config_.pre_us))
                return;
            current->last_sequence = item->sequence;
            if (!current->frames.push(item)) {
                // 编码队列溢出终止整段录像，不能把缺失尾段的文件当作保存成功。
                current->abort = true;
                report("recording_overflow", current->output);
            }
        };
        FrameRef frame;
        while (!stopping_) {
            // 先收一批已排队帧再处理触发，保证刚发生事件前的帧进入快照；
            // 每批最多 32 帧，持续输入也不会让触发处理一直得不到执行机会。
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
                // 窗口以事件发生时间为基准，而非本线程开始处理该事件的时间。
                now = monotonic_us();
                if (trigger_time > now || !trigger_time || now - trigger_time >= config_.post_us) {
                    report("recording_rejected", "stale_or_future_trigger");
                    continue;
                }
                // 超过旧窗口的触发属于新事件，不能复活已经结束或达到时长上限的录像。
                if (window.active() && trigger_time >= window.end())
                    finish_session();
                if (window.active()) {
                    window.trigger(trigger_time);
                    // 续期可能延迟处理；把已进 ring、但曾超出旧截止时间的帧补入会话。
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
                // 取前段及已缓存的后段引用；触发到达较晚时，后段可能已经进 ring。
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
