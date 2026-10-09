// 纯检测后处理，不依赖 STAI 设备：校验候选框 -> 分数排序 -> 同类别 NMS。
#include "vision/ssd_postprocess.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace helmet {
namespace {
float intersection_over_union(const detect_result_t &a, const detect_result_t &b) {
    const float area_a = (a.x1 - a.x0) * (a.y1 - a.y0);
    const float area_b = (b.x1 - b.x0) * (b.y1 - b.y0);
    const float width = std::max(0.0f, std::min(a.x1, b.x1) - std::max(a.x0, b.x0));
    const float height = std::max(0.0f, std::min(a.y1, b.y1) - std::max(a.y0, b.y0));
    const float intersection = width * height;
    return intersection / (area_a + area_b - intersection);
}
} // namespace

std::vector<detect_result_t> suppress_ssd_boxes(const std::vector<float> &boxes,
                                                const std::vector<int> &class_indices,
                                                const std::vector<float> &scores,
                                                float iou_threshold) {
    if (boxes.size() % 4 || class_indices.size() != boxes.size() / 4 ||
        scores.size() != class_indices.size() || !std::isfinite(iou_threshold) ||
        iou_threshold < 0 || iou_threshold > 1)
        throw std::invalid_argument("invalid SSD postprocessing inputs");
    std::vector<detect_result_t> candidates;
    for (size_t i = 0; i < scores.size(); ++i) {
        detect_result_t object{class_indices[i], scores[i],        boxes[i * 4],
                               boxes[i * 4 + 1], boxes[i * 4 + 2], boxes[i * 4 + 3]};
        if (object.class_index <= 0 || !std::isfinite(object.score) || object.score < 0 ||
            object.score > 1 || !std::isfinite(object.x0) || !std::isfinite(object.y0) ||
            !std::isfinite(object.x1) || !std::isfinite(object.y1) || object.x1 <= object.x0 ||
            object.y1 <= object.y0)
            continue;
        candidates.push_back(object);
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const auto &a, const auto &b) { return a.score > b.score; });
    std::vector<detect_result_t> result;
    for (const auto &candidate : candidates) {
        // 已保留的框分数不低于当前框；只在同类别间抑制，避免车辆框误删重叠的行人框。
        const bool duplicate = std::any_of(result.begin(), result.end(), [&](const auto &kept) {
            return kept.class_index == candidate.class_index &&
                   intersection_over_union(kept, candidate) > iou_threshold;
        });
        if (!duplicate)
            result.push_back(candidate);
    }
    return result;
}
} // namespace helmet
