/**
 * npu_detect.cpp - STM32MP2 NPU 目标检测封装 (stai_mpu)
 *
 * 数据流:
 *   NpuDetector()    -> 加载 .nb 模型, 分配输入张量内存, 读取标签
 *   detect(rgb_buf)  -> 预处理 (归一化/量化) -> NPU 推理 -> 后处理 (NMS) -> 返回检测框
 *   get_input_width/height() -> 供调用者 resize 图像到模型输入尺寸
 *
 * 调用位置: radar_fusion.cpp 主循环, 每 10 帧摄像头图像调用一次
 */
#include "npu_detect.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <sys/time.h>
#include <chrono>
#include <stdexcept>

// The vendor base has a non-virtual destructor. Own the exact final type so
// deleting the network never relies on polymorphic destruction.
class NpuNetwork final : public stai_mpu_network {
  public:
    using stai_mpu_network::stai_mpu_network;
};

NpuDetector::NpuDetector(const char *model_path, const char *labels_path, float confidence_thresh,
                         float iou_thresh)
    : confidence_thresh_(confidence_thresh), iou_thresh_(iou_thresh), input_mean_(127.5f),
      input_std_(127.5f) {
    /* Load model */
    printf("[NPU] Loading model: %s\n", model_path);
    model_ = std::make_unique<NpuNetwork>(model_path, true);

    std::vector<stai_mpu_tensor> input_infos = model_->get_input_infos();
    std::vector<stai_mpu_tensor> output_infos = model_->get_output_infos();

    /* Get input shape */
    if (input_infos.size() != 1 || output_infos.size() != 3)
        throw std::runtime_error("Expected one NHWC input and three SSD outputs");
    std::vector<int> shape = input_infos[0].get_shape();
    if (shape.size() != 4 || shape[0] != 1 || shape[1] <= 0 || shape[1] > 4096 || shape[2] <= 0 ||
        shape[2] > 4096 || shape[3] != 3)
        throw std::runtime_error("Unsupported NPU input shape");
    input_height_ = shape[1];
    input_width_ = shape[2];
    input_channels_ = shape[3];
    input_size_bytes_ = input_width_ * input_height_ * input_channels_;

    printf("[NPU] Input: %dx%dx%d (dtype=%d), size=%d bytes, %d outputs\n", input_width_,
           input_height_, input_channels_, (int)input_infos[0].get_dtype(), input_size_bytes_,
           (int)output_infos.size());

    /* Allocate input buffers */
    input_tensor_u8_.resize(input_size_bytes_);
    input_tensor_f32_.resize(input_size_bytes_);

    /* Load labels */
    if (load_labels(labels_path) != 0) {
        throw std::runtime_error("Cannot load NPU labels");
    }
    printf("[NPU] Loaded %zu labels\n", labels_.size());
}

/**
 * @brief 释放输入张量内存
 */
NpuDetector::~NpuDetector() = default;

/**
 * @brief 从文本文件加载类别标签
 * @param filename 标签文件路径，每行一个类别名
 * @return 0 成功，-1 失败
 */
int NpuDetector::load_labels(const char *filename) {
    std::ifstream file(filename);
    if (!file) {
        fprintf(stderr, "[NPU] Cannot open labels file: %s\n", filename);
        return -1;
    }
    labels_.clear();
    std::string line;
    while (std::getline(file, line)) {
        labels_.push_back(line);
    }
    return 0;
}

/**
 * @brief 根据类别索引获取标签名
 * @param class_index 模型输出的类别索引
 * @return 对应标签名；索引越界时返回 "unknown"
 */
const std::string &NpuDetector::get_label(int class_index) const {
    static std::string unknown = "unknown";
    if (class_index >= 0 && (size_t)class_index < labels_.size())
        return labels_[class_index];
    return unknown;
}

/**
 * @brief 对 RGB 图像执行 NPU 推理并返回检测结果
 * @param rgb_data uint8_t 类型的交错 RGB24 图像数据，尺寸须为模型输入尺寸
 * @return 包含检测框列表和推理耗时的结构体
 *
 * 流程：复制图像 -> 根据模型输入类型做归一化/量化 -> NPU 推理 ->
 *       SSD MobileNet V2 后处理（分数过滤、框解码、NMS）。
 */
frame_results_t NpuDetector::detect(const uint8_t *rgb_data) {
    frame_results_t results;
    results.inference_time_ms = 0.0f;

    /* Copy and preprocess */
    memcpy(input_tensor_u8_.data(), rgb_data, input_size_bytes_);

    std::vector<stai_mpu_tensor> input_infos = model_->get_input_infos();
    bool is_float = (input_infos[0].get_dtype() == stai_mpu_dtype::STAI_MPU_DTYPE_FLOAT32);

    if (is_float) {
        for (int i = 0; i < input_size_bytes_; i++) {
            input_tensor_f32_[i] = (input_tensor_u8_[i] - input_mean_) / input_std_;
        }
        model_->set_input(0, input_tensor_f32_.data());
    } else {
        model_->set_input(0, input_tensor_u8_.data());
    }

    /* Run inference */
    auto start = std::chrono::steady_clock::now();
    if (!model_->run())
        throw std::runtime_error("NPU inference failed");
    results.inference_time_ms =
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();

    /* --- SSD MobileNet V2 post-processing --- */
    std::vector<stai_mpu_tensor> output_infos = model_->get_output_infos();
    std::vector<int> output_shape_0 = output_infos[0].get_shape();
    auto boxes_shape = output_infos[1].get_shape();
    auto anchors_shape = output_infos[2].get_shape();
    if (output_shape_0.size() != 3 || boxes_shape.size() != 3 || anchors_shape.size() != 3 ||
        output_shape_0[1] <= 0 || output_shape_0[1] > 100000 || output_shape_0[2] <= 1 ||
        output_shape_0[2] > 1000 || boxes_shape[1] != output_shape_0[1] || boxes_shape[2] != 4 ||
        anchors_shape[1] != output_shape_0[1] || anchors_shape[2] != 4)
        throw std::runtime_error("Unsupported SSD output shapes");
    int nboxes = output_shape_0[1];
    int nclasses = output_shape_0[2];

    const bool owns_output =
        model_->get_backend_engine() == stai_mpu_backend_engine::STAI_MPU_OVX_NPU_ENGINE;
    auto release = [owns_output](float *output) {
        if (owns_output)
            free(output);
    };
    std::unique_ptr<float, decltype(release)> classes_owner(
        static_cast<float *>(model_->get_output(0)), release);
    std::unique_ptr<float, decltype(release)> boxes_owner(
        static_cast<float *>(model_->get_output(1)), release);
    std::unique_ptr<float, decltype(release)> anchors_owner(
        static_cast<float *>(model_->get_output(2)), release);
    float *class_pred = classes_owner.get();
    float *box_encoded = boxes_owner.get();
    float *anchors = anchors_owner.get();
    if (!class_pred || !box_encoded || !anchors)
        throw std::runtime_error("NPU returned an empty output tensor");

    int ncoords = output_infos[1].get_shape()[2];

    /* Filter by score */
    std::vector<int> filtered_idx =
        filter_by_score(class_pred, nboxes, nclasses, confidence_thresh_);

    /* Build filtered vectors */
    std::vector<float> filtered_boxes(filtered_idx.size() * ncoords);
    std::vector<float> filtered_anchors(filtered_idx.size() * ncoords);
    std::vector<float> filtered_scores(filtered_idx.size() * nclasses);

    for (size_t i = 0; i < filtered_idx.size(); i++) {
        int row = filtered_idx[i];
        memcpy(&filtered_boxes[i * ncoords], &box_encoded[row * ncoords], ncoords * sizeof(float));
        memcpy(&filtered_anchors[i * ncoords], &anchors[row * ncoords], ncoords * sizeof(float));
        memcpy(&filtered_scores[i * nclasses], &class_pred[row * nclasses],
               nclasses * sizeof(float));
    }

    /* Decode */
    std::vector<float> decoded = bb_decoding(filtered_boxes, filtered_anchors);

    /* Recover score info */
    std::vector<float> hi_scores;
    std::vector<int> class_indices;
    recover_score_info(filtered_scores, filtered_idx.size(), nclasses, hi_scores, class_indices);

    /* NMS */
    results.objects = nms(decoded, class_indices, hi_scores, iou_thresh_);

    return results;
}

/* ================ Post-processing helpers ================ */

/**
 * @brief 按置信度阈值过滤检测框
 * @param predictions 类别预测分数矩阵，形状 [rows, cols]
 * @param rows        锚框数量
 * @param cols        类别数量（含背景类 0）
 * @param threshold   置信度阈值
 * @return 通过过滤的锚框索引列表
 *
 * 跳过背景类（索引 0），只要某个前景类分数超过阈值即保留该锚框。
 */
std::vector<int> NpuDetector::filter_by_score(float *predictions, int rows, int cols,
                                              float threshold) {
    std::vector<int> filtered;
    for (int i = 0; i < rows; i++) {
        for (int j = 1; j < cols; j++) {
            if (predictions[i * cols + j] > threshold) {
                filtered.push_back(i);
                break;
            }
        }
    }
    return filtered;
}

/**
 * @brief 将模型输出的相对编码框解码为绝对坐标框
 * @param encoded  模型输出的编码框，每个框 4 个浮点数
 * @param anchors  对应锚框坐标，每个框 4 个浮点数
 * @return 解码后的绝对坐标框列表
 *
 * 解码公式：decoded = encoded * anchor_size + anchor_min
 */
std::vector<float> NpuDetector::bb_decoding(const std::vector<float> &encoded,
                                            const std::vector<float> &anchors) {
    std::vector<float> decoded(encoded.size());
    int n = encoded.size() / 4;
    for (int i = 0; i < n; i++) {
        float ax = anchors[i * 4], ay = anchors[i * 4 + 1];
        float ax2 = anchors[i * 4 + 2], ay2 = anchors[i * 4 + 3];
        float bx = encoded[i * 4], by = encoded[i * 4 + 1];
        float bx2 = encoded[i * 4 + 2], by2 = encoded[i * 4 + 3];

        float w = ax2 - ax, h = ay2 - ay;
        decoded[i * 4] = bx * w + ax;
        decoded[i * 4 + 1] = by * h + ay;
        decoded[i * 4 + 2] = bx2 * w + ax2;
        decoded[i * 4 + 3] = by2 * h + ay2;
    }
    return decoded;
}

/**
 * @brief 计算两个检测框的交并比（IoU）
 * @param a 检测框 A
 * @param b 检测框 B
 * @return IoU 值，范围 [0, 1]
 */
float NpuDetector::iou(const detect_result_t &a, const detect_result_t &b) {
    float areaA = (a.x1 - a.x0) * (a.y1 - a.y0);
    float areaB = (b.x1 - b.x0) * (b.y1 - b.y0);
    if (areaA <= 0 || areaB <= 0)
        return 0;

    float ix = std::max(a.x0, b.x0);
    float iy = std::max(a.y0, b.y0);
    float ix2 = std::min(a.x1, b.x1);
    float iy2 = std::min(a.y1, b.y1);
    float iarea = std::max(0.0f, ix2 - ix) * std::max(0.0f, iy2 - iy);
    return iarea / (areaA + areaB - iarea);
}

/**
 * @brief 非极大值抑制（NMS）
 * @param boxes         解码后的检测框坐标，每个框 4 个浮点数
 * @param class_indices 每个框对应的类别索引
 * @param scores        每个框对应的最佳类别分数
 * @param iou_threshold 抑制阈值，IoU 超过该值的低分框将被过滤
 * @return 抑制后的检测框列表
 */
std::vector<detect_result_t> NpuDetector::nms(const std::vector<float> &boxes,
                                              const std::vector<int> &class_indices,
                                              const std::vector<float> &scores,
                                              float iou_threshold) {
    size_t n = boxes.size() / 4;
    std::vector<detect_result_t> enriched(n);
    for (size_t i = 0; i < n; i++) {
        enriched[i].x0 = boxes[i * 4];
        enriched[i].y0 = boxes[i * 4 + 1];
        enriched[i].x1 = boxes[i * 4 + 2];
        enriched[i].y1 = boxes[i * 4 + 3];
        enriched[i].score = scores[i];
        enriched[i].class_index = class_indices[i];
    }

    std::vector<int> indices(n);
    for (size_t i = 0; i < n; i++)
        indices[i] = i;
    std::sort(indices.begin(), indices.end(),
              [&](int a, int b) { return enriched[a].score > enriched[b].score; });

    std::vector<bool> suppressed(n, false);
    std::vector<detect_result_t> result;

    for (size_t i = 0; i < n; i++) {
        if (suppressed[indices[i]])
            continue;
        int idx = indices[i];
        result.push_back(enriched[idx]);
        for (size_t j = i + 1; j < n; j++) {
            if (!suppressed[indices[j]] &&
                iou(enriched[idx], enriched[indices[j]]) > iou_threshold) {
                suppressed[indices[j]] = true;
            }
        }
    }
    return result;
}

/**
 * @brief 从类别分数矩阵中恢复每个框的最高分及其类别索引
 * @param scores        类别分数矩阵，形状 [nboxes, nclasses]
 * @param nboxes        框数量
 * @param nclasses      类别数量
 * @param hi_scores     输出每个框的最高类别分数
 * @param class_indices 输出每个框对应的类别索引
 *
 * 跳过背景类（索引 0），只考虑前景类。
 */
void NpuDetector::recover_score_info(const std::vector<float> &scores, int nboxes, int nclasses,
                                     std::vector<float> &hi_scores,
                                     std::vector<int> &class_indices) {
    for (int box = 0; box < nboxes; box++) {
        int start = box * nclasses;
        auto max_it =
            std::max_element(scores.begin() + start + 1, scores.begin() + start + nclasses);
        hi_scores.push_back(*max_it);
        class_indices.push_back(std::distance(scores.begin() + start, max_it));
    }
}
