/**
 * npu_detect.h - STM32MP2 NPU 目标检测接口 (stai_mpu)
 *
 * 数据流:
 *   NpuDetector(model, labels) -> 加载 .nb 模型，读取标签
 *   detect(rgb_data)           -> 预处理 -> NPU 推理 -> 后处理 -> 检测框列表
 *   get_input_width/height()   -> 供调用者 resize 图像到模型输入尺寸
 *
 * 调用位置:
 *   - src/runtime/inference_worker.cpp（独立 NPU 线程）
 */
#ifndef NPU_DETECT_H
#define NPU_DETECT_H

#include <string>
#include <vector>
#include <memory>
#include "vision/detection_types.hpp"
#include "stai_mpu_network.h"

class NpuNetwork;

class NpuDetector {
  public:
    NpuDetector(const char *model_path, const char *labels_path, float confidence_thresh = 0.70f,
                float iou_thresh = 0.45f);
    ~NpuDetector();
    NpuDetector(const NpuDetector &) = delete;
    NpuDetector &operator=(const NpuDetector &) = delete;

    int get_input_width() const {
        return input_width_;
    }

    int get_input_height() const {
        return input_height_;
    }

    int get_input_channels() const {
        return input_channels_;
    }

    /** Run inference on RGB image data (uint8_t, interleaved RGB) */
    frame_results_t detect(const uint8_t *rgb_data);

    /** Get label name for a class index */
    const std::string &get_label(int class_index) const;

  private:
    int load_labels(const char *filename);

    /* --- post-processing (from 正点原子 SsdMobilenetpp) --- */
    std::vector<int> filter_by_score(float *predictions, int rows, int cols, float threshold);
    std::vector<float> bb_decoding(const std::vector<float> &encoded,
                                   const std::vector<float> &anchors);
    void recover_score_info(const std::vector<float> &scores, int nboxes, int nclasses,
                            std::vector<float> &hi_scores, std::vector<int> &class_indices);

    std::unique_ptr<NpuNetwork> model_;
    std::vector<std::string> labels_;

    int input_width_;
    int input_height_;
    int input_channels_;
    int input_size_bytes_;

    float confidence_thresh_;
    float iou_thresh_;
    float input_mean_;
    float input_std_;

    std::vector<uint8_t> input_tensor_u8_;
    std::vector<float> input_tensor_f32_;
};

#endif /* NPU_DETECT_H */
