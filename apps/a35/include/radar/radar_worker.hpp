#pragma once
#include "app/services.hpp"
#include "runtime/frame_pipeline.hpp"
#include <string>

// 结果按值传递，不借用串口接收区；timestamp_us 是 A35 收到完整报告时的单调时间。
struct RadarObservation {
    radar_result_t result{};
    uint64_t timestamp_us = 0;
};

void radar_receive_loop(const std::string &device,
                        helmet::BoundedQueue<RadarObservation> &observations);
