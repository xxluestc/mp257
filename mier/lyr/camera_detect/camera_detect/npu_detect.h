/**
 * npu_detect.h - STM32MP2 NPU 目标检测接口 (stai_mpu)
 *
 * 数据流:
 *   NpuDetector(model, labels) -> 加载 .nb 模型，读取标签
 *   detect(rgb_data)           -> 预处理 -> NPU 推理 -> 后处理 -> 检测框列表
 *   get_input_width/height()   -> 供调用者 resize 图像到模型输入尺寸
 *
 * 调用位置:
 *   - main.cpp        (camera_detect 单检测程序)
 *   - fusion_main.cpp (fusion_detect 雷达+NPU融合验证)
 *   - radar_fusion.cpp (主程序道路用户检测与 DVR 触发)
 */
#ifndef NPU_DETECT_H
#define NPU_DETECT_H

#include <string>
#include <vector>
#include <memory>
#include "stai_mpu_network.h"

/** 单个检测结果 */
typedef struct {
    int class_index;   /* COCO 类别索引 */
    float score;       /* 置信度 */
    float x0, y0;      /* 左上角归一化坐标 */
    float x1, y1;      /* 右下角归一化坐标 */
} detect_result_t;

/** 单帧推理结果集合 */
typedef struct {
    std::vector<detect_result_t> objects;
    float inference_time_ms;
} frame_results_t;

class NpuDetector {
public:
    NpuDetector(const char *model_path, const char *labels_path,
                float confidence_thresh = 0.70f, float iou_thresh = 0.45f);
    ~NpuDetector();

    int get_input_width()  const { return input_width_; }
    int get_input_height() const { return input_height_; }
    int get_input_channels() const { return input_channels_; }

    /** Run inference on RGB image data (uint8_t, interleaved RGB) */
    frame_results_t detect(const uint8_t *rgb_data);

    /** Get label name for a class index */
    const std::string& get_label(int class_index) const;

private:
    int load_labels(const char *filename);

    /* --- post-processing (from 正点原子 SsdMobilenetpp) --- */
    std::vector<int> filter_by_score(float *predictions, int rows, int cols, float threshold);
    std::vector<float> bb_decoding(const std::vector<float> &encoded, const std::vector<float> &anchors);
    float iou(const detect_result_t &a, const detect_result_t &b);
    std::vector<detect_result_t> nms(const std::vector<float> &boxes,
                                     const std::vector<int> &class_indices,
                                     const std::vector<float> &scores,
                                     float iou_threshold);
    void recover_score_info(const std::vector<float> &scores, int nboxes, int nclasses,
                            std::vector<float> &hi_scores, std::vector<int> &class_indices);

    std::unique_ptr<stai_mpu_network> model_;
    std::vector<std::string> labels_;

    int input_width_;
    int input_height_;
    int input_channels_;
    int input_size_bytes_;

    float confidence_thresh_;
    float iou_thresh_;
    float input_mean_;
    float input_std_;

    uint8_t *input_tensor_u8_;
    float   *input_tensor_f32_;
};

#endif /* NPU_DETECT_H */