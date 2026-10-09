#pragma once

#include <vector>

struct detect_result_t {
    int class_index; // COCO index, including background at index 0.
    float score;
    float x0, y0;
    float x1, y1;
};

struct frame_results_t {
    std::vector<detect_result_t> objects;
    float inference_time_ms = 0;
};
