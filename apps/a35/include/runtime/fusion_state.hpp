#pragma once
#include <algorithm>
#include <cstdint>

namespace helmet {
// 由 Fusion 独占的视觉确认状态：连续两次阳性确认，连续三次阴性解除，抑制单帧抖动。
class VisionGate {
    uint64_t timestamp_ = 0;
    unsigned positive_ = 0;
    unsigned negative_ = 0;
    bool confirmed_ = false;

  public:
    static constexpr uint64_t freshness_us = 2000000;

    void update(uint64_t timestamp, bool road_user) {
        // 忽略重复/乱序结果；采样间隔过大时重新累计，不能拼接两段不连续的观察。
        if (timestamp <= timestamp_)
            return;
        if (timestamp_ && timestamp - timestamp_ > freshness_us) {
            positive_ = negative_ = 0;
            confirmed_ = false;
        }
        timestamp_ = timestamp;
        if (road_user) {
            positive_ = std::min(positive_ + 1, 2U);
            negative_ = 0;
            if (positive_ == 2)
                confirmed_ = true;
        } else {
            positive_ = 0;
            negative_ = std::min(negative_ + 1, 3U);
            if (negative_ == 3)
                confirmed_ = false;
        }
    }

    bool fresh(uint64_t now) const {
        return timestamp_ && now >= timestamp_ && now - timestamp_ <= freshness_us;
    }

    bool confirmed() const {
        return confirmed_;
    }

    bool alert(bool radar_danger, bool camera_available, uint64_t now) const {
        // 摄像头或推理结果失效时采用雷达风险；视觉可用时要求道路使用者已确认。
        return radar_danger && (!camera_available || !fresh(now) || confirmed_);
    }
};
} // namespace helmet
