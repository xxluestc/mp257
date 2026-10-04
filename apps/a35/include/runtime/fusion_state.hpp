#pragma once
#include <algorithm>
#include <cstdint>

namespace helmet {
// Confirmation belongs to Fusion, never to the NPU, camera or encoder thread.
class VisionGate {
    uint64_t timestamp_ = 0;
    unsigned positive_ = 0;
    unsigned negative_ = 0;
    bool confirmed_ = false;

  public:
    static constexpr uint64_t freshness_us = 2000000;

    void update(uint64_t timestamp, bool road_user) {
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
        return radar_danger && (!camera_available || !fresh(now) || confirmed_);
    }
};
} // namespace helmet
