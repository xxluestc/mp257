#pragma once
#include "app/services.hpp"
#include "runtime/frame_pipeline.hpp"
#include <string>

struct RadarObservation {
    radar_result_t result{};
    uint64_t timestamp_us = 0;
};

void radar_receive_loop(const std::string &device,
                        helmet::BoundedQueue<RadarObservation> &observations);
