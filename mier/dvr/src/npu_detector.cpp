/*
 * npu_detector.cpp - NPU检测器实现
 *
 * 从 systemui_src/ssd_mobilenet_v2/include/staimpuwrapper.cpp
 * 和 ssdmobilenetpp.cpp 剥离Qt依赖而来
 *
 * 关键改动：
 *   - 移除 QObject 继承
 *   - 移除 QVector → std::vector
 *   - 移除 signals/slots
 *   - 使用 PIMPL 隐藏 stai_mpu 头文件依赖
 *   - 添加图像缩放（支持任意输入尺寸缩放到模型尺寸）
 */

#include "npu_detector.hpp"

#ifndef NO_NPU_SUPPORT

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <numeric>
#include <cmath>

/* 开发板SDK头文件 */
#include "stai_mpu_network.h"

/* ===================================================================
 * PIMPL 实现：隐藏 stai_mpu 依赖
 * =================================================================== */
struct NpuDetector::Impl {
    std::unique_ptr<stai_mpu_network>  model;
    std::vector<stai_mpu_tensor>       input_infos;
    std::vector<stai_mpu_tensor>       output_infos;
    std::vector<int>                   input_shape;
    std::vector<int>                   output_shape;
    uint8_t                           *input_tensor_int;
    float                             *input_tensor_f;
    int                                num_inputs;
    int                                num_outputs;
    int                                size_in_bytes;
    bool                               floating_model;
    std::string                        model_type;

    /* 后处理相关 */
    std::vector<float> anchors;        /* 首次加载后缓存 */

    Impl() : input_tensor_int(nullptr), input_tensor_f(nullptr),
             num_inputs(0), num_outputs(0), size_in_bytes(0),
             floating_model(false), model_type("ssd_mobilenet_v2") {}
    ~Impl() {
        delete[] input_tensor_int;
        delete[] input_tensor_f;
    }
};

/* ===================================================================
 * 构造函数 / 析构函数
 * =================================================================== */
NpuDetector::NpuDetector()
    : m_initialized(false), m_inputWidth(256), m_inputHeight(256),
      m_inputChannels(3), m_inferenceTime(0.0f)
{
    m_pImpl = std::make_unique<Impl>();
}

NpuDetector::~NpuDetector() = default;

/* ===================================================================
 * 工具函数
 * =================================================================== */
double NpuDetector::GetTimeMs(struct timeval t)
{
    return (double)(t.tv_sec * 1000 + t.tv_usec / 1000);
}

/* 最近邻缩放 RGB 图像 */
static void ResizeRGB(const uint8_t *src, int src_w, int src_h,
                      uint8_t *dst, int dst_w, int dst_h)
{
    for (int y = 0; y < dst_h; y++) {
        int src_y = y * src_h / dst_h;
        for (int x = 0; x < dst_w; x++) {
            int src_x = x * src_w / dst_w;
            int src_idx = (src_y * src_w + src_x) * 3;
            int dst_idx = (y * dst_w + x) * 3;
            dst[dst_idx + 0] = src[src_idx + 0];
            dst[dst_idx + 1] = src[src_idx + 1];
            dst[dst_idx + 2] = src[src_idx + 2];
        }
    }
}

/* ===================================================================
 * 标签文件读取
 * =================================================================== */
bool NpuDetector::ReadLabelsFile(const std::string &file_name)
{
    std::ifstream file(file_name);
    if (!file) {
        std::cerr << "[NPU] Labels file not found: " << file_name << std::endl;
        return false;
    }
    mLabels.clear();
    std::string line;
    while (std::getline(file, line)) {
        mLabels.push_back(line);
    }
    /* 填充到16的倍数（模型要求） */
    while (mLabels.size() % 16) {
        mLabels.emplace_back("");
    }
    std::cout << "[NPU] Loaded " << (line.empty() ? 0 : mLabels.size())
              << " labels from " << file_name << std::endl;
    return true;
}

/* ===================================================================
 * 初始化
 * =================================================================== */
bool NpuDetector::Initialize(const NpuDetectorConfig &config)
{
    m_config = config;

    if (config.model_path.empty()) {
        std::cerr << "[NPU] No model file specified" << std::endl;
        return false;
    }

    std::cout << "[NPU] Loading model: " << config.model_path << std::endl;

    try {
        /* 加载模型 */
        m_pImpl->model.reset(new stai_mpu_network(config.model_path, true));
        m_pImpl->input_infos  = m_pImpl->model->get_input_infos();
        m_pImpl->output_infos = m_pImpl->model->get_output_infos();
        m_pImpl->num_inputs   = m_pImpl->model->get_num_inputs();
        m_pImpl->num_outputs  = m_pImpl->model->get_num_outputs();
    } catch (const std::exception &e) {
        std::cerr << "[NPU] Model loading failed: " << e.what() << std::endl;
        return false;
    }

    /* 获取输入尺寸 */
    for (int i = 0; i < m_pImpl->num_inputs; i++) {
        m_pImpl->input_shape = m_pImpl->input_infos[i].get_shape();
    }
    /* input_shape: [batch, width, height, channels] */
    m_inputWidth    = m_pImpl->input_shape[1];
    m_inputHeight   = m_pImpl->input_shape[2];
    m_inputChannels = m_pImpl->input_shape[3];

    m_pImpl->size_in_bytes = m_inputWidth * m_inputHeight * m_inputChannels;
    m_pImpl->input_tensor_int = new uint8_t[m_pImpl->size_in_bytes];
    m_pImpl->input_tensor_f   = new float[m_pImpl->size_in_bytes];

    /* 判断模型类型 */
    if (m_pImpl->input_infos[0].get_dtype() == stai_mpu_dtype::STAI_MPU_DTYPE_FLOAT32) {
        m_pImpl->floating_model = true;
    } else {
        m_pImpl->floating_model = false;
    }

    /* 读取标签 */
    if (!config.labels_path.empty()) {
        ReadLabelsFile(config.labels_path);
    }

    m_initialized = true;

    std::cout << "[NPU] Initialized: " << m_inputWidth << "x" << m_inputHeight
              << "x" << m_inputChannels
              << " (" << (m_pImpl->floating_model ? "FP32" : "INT8") << ")"
              << std::endl;
    return true;
}

/* ===================================================================
 * 后处理：Filter by score
 * =================================================================== */
static std::vector<int> FilterByScore(const float *predictions,
                                       int rows, int cols,
                                       float confidence_thresh)
{
    std::vector<int> filtered;
    for (int i = 0; i < rows; i++) {
        for (int j = 1; j < cols; j++) {
            if (predictions[i * cols + j] > confidence_thresh) {
                filtered.push_back(i);
                break;
            }
        }
    }
    return filtered;
}

/* ===================================================================
 * 后处理：Bounding Box Decoding
 * =================================================================== */
static std::vector<float> BBDecoding(const std::vector<float> &encoded_bbox,
                                      const std::vector<float> &anchors)
{
    std::vector<float> decoded(encoded_bbox.size());
    int n = (int)encoded_bbox.size() / 4;
    for (int i = 0; i < n; i++) {
        float a_xmin = anchors[i * 4];
        float a_ymin = anchors[i * 4 + 1];
        float a_xmax = anchors[i * 4 + 2];
        float a_ymax = anchors[i * 4 + 3];

        float w = a_xmax - a_xmin;
        float h = a_ymax - a_ymin;

        decoded[i * 4]     = encoded_bbox[i * 4] * w + a_xmin;
        decoded[i * 4 + 1] = encoded_bbox[i * 4 + 1] * h + a_ymin;
        decoded[i * 4 + 2] = encoded_bbox[i * 4 + 2] * w + a_xmax;
        decoded[i * 4 + 3] = encoded_bbox[i * 4 + 3] * h + a_ymax;
    }
    return decoded;
}

/* ===================================================================
 * 后处理：IoU
 * =================================================================== */
static float CalcIoU(const DetectedObject &a, const DetectedObject &b)
{
    float areaA = (a.x1 - a.x0) * (a.y1 - a.y0);
    if (areaA <= 0.0f) return 0.0f;
    float areaB = (b.x1 - b.x0) * (b.y1 - b.y0);
    if (areaB <= 0.0f) return 0.0f;

    float ix = std::max(a.x0, b.x0);
    float iy = std::max(a.y0, b.y0);
    float iX = std::min(a.x1, b.x1);
    float iY = std::min(a.y1, b.y1);

    float iArea = std::max(0.0f, iX - ix) * std::max(0.0f, iY - iy);
    return iArea / (areaA + areaB - iArea);
}

/* ===================================================================
 * 后处理：NMS
 * =================================================================== */
static std::vector<DetectedObject> NMS(
    const std::vector<float> &bb_predictions,
    const std::vector<int>   &class_index,
    const std::vector<float> &filtered_scores,
    float iou_threshold)
{
    size_t num_boxes = bb_predictions.size() / 4;
    std::vector<DetectedObject> boxes(num_boxes);

    for (size_t i = 0; i < num_boxes; i++) {
        boxes[i].x0 = bb_predictions[i * 4];
        boxes[i].y0 = bb_predictions[i * 4 + 1];
        boxes[i].x1 = bb_predictions[i * 4 + 2];
        boxes[i].y1 = bb_predictions[i * 4 + 3];
        boxes[i].score = filtered_scores[i];
        boxes[i].class_id = class_index[i];
    }

    /* 按分数降序排列 */
    std::vector<int> indices(num_boxes);
    std::iota(indices.begin(), indices.end(), 0);
    std::sort(indices.begin(), indices.end(), [&boxes](int a, int b) {
        return boxes[a].score > boxes[b].score;
    });

    std::vector<bool> suppressed(num_boxes, false);
    std::vector<DetectedObject> final_output;

    for (size_t i = 0; i < num_boxes; i++) {
        if (suppressed[indices[i]]) continue;
        int idx = indices[i];
        final_output.push_back(boxes[idx]);
        for (size_t j = i + 1; j < num_boxes; j++) {
            if (!suppressed[indices[j]] &&
                CalcIoU(boxes[idx], boxes[indices[j]]) > iou_threshold) {
                suppressed[indices[j]] = true;
            }
        }
    }
    return final_output;
}

/* ===================================================================
 * 后处理：提取最高分和类别
 * =================================================================== */
static void RecoverScoreInfo(const std::vector<float> &scores,
                              int number_of_boxes,
                              int number_of_classes,
                              std::vector<float> &highest_scores,
                              std::vector<int> &class_indices)
{
    for (int box = 0; box < number_of_boxes; box++) {
        int start = box * number_of_classes;
        auto max_it = std::max_element(scores.begin() + start + 1,
                                        scores.begin() + start + number_of_classes);
        highest_scores.push_back(*max_it);
        class_indices.push_back(std::distance(scores.begin() + start, max_it));
    }
}

/* ===================================================================
 * 运行推理
 * =================================================================== */
std::vector<DetectedObject> NpuDetector::RunInference(
    const uint8_t *rgb_data, int width, int height)
{
    std::vector<DetectedObject> results;

    if (!m_initialized || !rgb_data) {
        return results;
    }

    struct timeval t_start, t_end;
    gettimeofday(&t_start, nullptr);

    /* 1. 缩放图像到模型输入尺寸 */
    uint8_t *input_data;
    if (width == m_inputWidth && height == m_inputHeight) {
        input_data = const_cast<uint8_t *>(rgb_data);
    } else {
        input_data = m_pImpl->input_tensor_int;
        ResizeRGB(rgb_data, width, height,
                  input_data, m_inputWidth, m_inputHeight);
    }

    /* 2. 设置输入并运行推理 */
    if (m_pImpl->floating_model) {
        for (int i = 0; i < m_pImpl->size_in_bytes; i++) {
            m_pImpl->input_tensor_f[i] =
                (float)(input_data[i] - m_config.input_mean) / m_config.input_std;
        }
        m_pImpl->model->set_input(0, m_pImpl->input_tensor_f);
    } else {
        m_pImpl->model->set_input(0, input_data);
    }

    m_pImpl->model->run();

    gettimeofday(&t_end, nullptr);
    m_inferenceTime = (float)(GetTimeMs(t_end) - GetTimeMs(t_start));

    /* 3. 后处理：SSD MobileNet V2 */
    std::vector<int> output_shape_0 = m_pImpl->output_infos[0].get_shape();
    std::vector<int> output_shape_1 = m_pImpl->output_infos[1].get_shape();
    int number_of_boxes = output_shape_0[1];
    int number_of_classes = output_shape_0[2];
    int number_of_coordinates = output_shape_1[2];

    float *box_encoded     = static_cast<float *>(m_pImpl->model->get_output(1));
    float *class_prediction = static_cast<float *>(m_pImpl->model->get_output(0));
    float *anchors         = static_cast<float *>(m_pImpl->model->get_output(2));

    /* Filter by score */
    std::vector<int> filtered_idx = FilterByScore(
        class_prediction, number_of_boxes, number_of_classes,
        m_config.confidence_thresh);

    if (filtered_idx.empty()) {
        /* 释放NPU输出内存 */
        if (m_pImpl->model->get_backend_engine() ==
            stai_mpu_backend_engine::STAI_MPU_OVX_NPU_ENGINE) {
            free(box_encoded);
            free(class_prediction);
            free(anchors);
        }
        return results;
    }

    /* 提取过滤后的数据 */
    std::vector<float> filtered_bb(filtered_idx.size() * number_of_coordinates);
    std::vector<float> filtered_anchors(filtered_idx.size() * number_of_coordinates);
    std::vector<float> filtered_class(filtered_idx.size() * number_of_classes);

    for (size_t i = 0; i < filtered_idx.size(); i++) {
        int row = filtered_idx[i];
        for (int j = 0; j < number_of_coordinates; j++) {
            filtered_bb[i * number_of_coordinates + j] =
                box_encoded[row * number_of_coordinates + j];
            filtered_anchors[i * number_of_coordinates + j] =
                anchors[row * number_of_coordinates + j];
        }
        for (int j = 0; j < number_of_classes; j++) {
            filtered_class[i * number_of_classes + j] =
                class_prediction[row * number_of_classes + j];
        }
    }

    /* Decode */
    std::vector<float> decoded_bb = BBDecoding(filtered_bb, filtered_anchors);

    /* Recover score info */
    std::vector<float> scores;
    std::vector<int> class_ids;
    RecoverScoreInfo(filtered_class, (int)filtered_idx.size(),
                     number_of_classes, scores, class_ids);

    /* NMS */
    results = NMS(decoded_bb, class_ids, scores, m_config.iou_threshold);

    /* 释放NPU输出内存 */
    if (m_pImpl->model->get_backend_engine() ==
        stai_mpu_backend_engine::STAI_MPU_OVX_NPU_ENGINE) {
        free(box_encoded);
        free(class_prediction);
        free(anchors);
    }

    return results;
}

#endif /* NO_NPU_SUPPORT */