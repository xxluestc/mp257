#pragma once

#include "vision/detection_types.hpp"

namespace helmet {
// Class-aware NMS is independent of the vendor runtime and rejects invalid
// coordinates/scores before sorting, which requires a strict weak ordering.
std::vector<detect_result_t> suppress_ssd_boxes(const std::vector<float> &boxes,
                                                const std::vector<int> &class_indices,
                                                const std::vector<float> &scores,
                                                float iou_threshold);
} // namespace helmet
