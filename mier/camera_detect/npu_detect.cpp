#include "npu_detect.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <sys/time.h>

NpuDetector::NpuDetector(const char *model_path, const char *labels_path,
                         float confidence_thresh, float iou_thresh)
    : confidence_thresh_(confidence_thresh), iou_thresh_(iou_thresh),
      input_mean_(127.5f), input_std_(127.5f),
      input_tensor_u8_(nullptr), input_tensor_f32_(nullptr)
{
    /* Load model */
    printf("[NPU] Loading model: %s\n", model_path);
    model_.reset(new stai_mpu_network(model_path, true));

    std::vector<stai_mpu_tensor> input_infos = model_->get_input_infos();
    std::vector<stai_mpu_tensor> output_infos = model_->get_output_infos();

    /* Get input shape */
    std::vector<int> shape = input_infos[0].get_shape();
    input_width_  = shape[1];
    input_height_ = shape[2];
    input_channels_ = shape[3];
    input_size_bytes_ = input_width_ * input_height_ * input_channels_;

    printf("[NPU] Input: %dx%dx%d (dtype=%d), size=%d bytes, %d outputs\n",
           input_width_, input_height_, input_channels_,
           (int)input_infos[0].get_dtype(),
           input_size_bytes_, (int)output_infos.size());

    /* Allocate input buffers */
    input_tensor_u8_ = new uint8_t[input_size_bytes_];
    input_tensor_f32_ = new float[input_size_bytes_];

    /* Load labels */
    if (load_labels(labels_path) != 0) {
        fprintf(stderr, "[NPU] Failed to load labels: %s\n", labels_path);
    }
    printf("[NPU] Loaded %zu labels\n", labels_.size());
}

NpuDetector::~NpuDetector()
{
    delete[] input_tensor_u8_;
    delete[] input_tensor_f32_;
}

int NpuDetector::load_labels(const char *filename)
{
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

const std::string& NpuDetector::get_label(int class_index) const
{
    static std::string unknown = "unknown";
    if (class_index >= 0 && (size_t)class_index < labels_.size())
        return labels_[class_index];
    return unknown;
}

frame_results_t NpuDetector::detect(const uint8_t *rgb_data)
{
    frame_results_t results;
    results.inference_time_ms = 0.0f;

    /* Copy and preprocess */
    memcpy(input_tensor_u8_, rgb_data, input_size_bytes_);

    std::vector<stai_mpu_tensor> input_infos = model_->get_input_infos();
    bool is_float = (input_infos[0].get_dtype() == stai_mpu_dtype::STAI_MPU_DTYPE_FLOAT32);

    if (is_float) {
        for (int i = 0; i < input_size_bytes_; i++) {
            input_tensor_f32_[i] = (input_tensor_u8_[i] - input_mean_) / input_std_;
        }
        model_->set_input(0, (const void*)input_tensor_f32_);
    } else {
        model_->set_input(0, (const void*)input_tensor_u8_);
    }

    /* Run inference */
    struct timeval start, end;
    gettimeofday(&start, nullptr);
    model_->run();
    gettimeofday(&end, nullptr);
    results.inference_time_ms = (end.tv_sec - start.tv_sec) * 1000.0f +
                                (end.tv_usec - start.tv_usec) / 1000.0f;

    /* --- SSD MobileNet V2 post-processing --- */
    std::vector<stai_mpu_tensor> output_infos = model_->get_output_infos();
    std::vector<int> output_shape_0 = output_infos[0].get_shape();
    int nboxes = output_shape_0[1];
    int nclasses = output_shape_0[2];

    float *class_pred = static_cast<float*>(model_->get_output(0));
    float *box_encoded = static_cast<float*>(model_->get_output(1));
    float *anchors = static_cast<float*>(model_->get_output(2));

    int ncoords = output_infos[1].get_shape()[2];

    /* Filter by score */
    std::vector<int> filtered_idx = filter_by_score(class_pred, nboxes, nclasses, confidence_thresh_);

    /* Build filtered vectors */
    std::vector<float> filtered_boxes(filtered_idx.size() * ncoords);
    std::vector<float> filtered_anchors(filtered_idx.size() * ncoords);
    std::vector<float> filtered_scores(filtered_idx.size() * nclasses);

    for (size_t i = 0; i < filtered_idx.size(); i++) {
        int row = filtered_idx[i];
        memcpy(&filtered_boxes[i * ncoords], &box_encoded[row * ncoords], ncoords * sizeof(float));
        memcpy(&filtered_anchors[i * ncoords], &anchors[row * ncoords], ncoords * sizeof(float));
        memcpy(&filtered_scores[i * nclasses], &class_pred[row * nclasses], nclasses * sizeof(float));
    }

    /* Decode */
    std::vector<float> decoded = bb_decoding(filtered_boxes, filtered_anchors);

    /* Recover score info */
    std::vector<float> hi_scores;
    std::vector<int> class_indices;
    recover_score_info(filtered_scores, filtered_idx.size(), nclasses, hi_scores, class_indices);

    /* NMS */
    results.objects = nms(decoded, class_indices, hi_scores, iou_thresh_);

    /* Free NPU output buffers if backend is NPU */
    if (model_->get_backend_engine() == stai_mpu_backend_engine::STAI_MPU_OVX_NPU_ENGINE) {
        free(class_pred);
        free(box_encoded);
        free(anchors);
    }

    return results;
}

/* ================ Post-processing helpers ================ */

std::vector<int> NpuDetector::filter_by_score(float *predictions, int rows, int cols, float threshold)
{
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

std::vector<float> NpuDetector::bb_decoding(const std::vector<float> &encoded, const std::vector<float> &anchors)
{
    std::vector<float> decoded(encoded.size());
    int n = encoded.size() / 4;
    for (int i = 0; i < n; i++) {
        float ax = anchors[i * 4], ay = anchors[i * 4 + 1];
        float ax2 = anchors[i * 4 + 2], ay2 = anchors[i * 4 + 3];
        float bx = encoded[i * 4], by = encoded[i * 4 + 1];
        float bx2 = encoded[i * 4 + 2], by2 = encoded[i * 4 + 3];

        float w = ax2 - ax, h = ay2 - ay;
        decoded[i * 4]     = bx * w + ax;
        decoded[i * 4 + 1] = by * h + ay;
        decoded[i * 4 + 2] = bx2 * w + ax2;
        decoded[i * 4 + 3] = by2 * h + ay2;
    }
    return decoded;
}

float NpuDetector::iou(const detect_result_t &a, const detect_result_t &b)
{
    float areaA = (a.x1 - a.x0) * (a.y1 - a.y0);
    float areaB = (b.x1 - b.x0) * (b.y1 - b.y0);
    if (areaA <= 0 || areaB <= 0) return 0;

    float ix = std::max(a.x0, b.x0);
    float iy = std::max(a.y0, b.y0);
    float ix2 = std::min(a.x1, b.x1);
    float iy2 = std::min(a.y1, b.y1);
    float iarea = std::max(0.0f, ix2 - ix) * std::max(0.0f, iy2 - iy);
    return iarea / (areaA + areaB - iarea);
}

std::vector<detect_result_t> NpuDetector::nms(const std::vector<float> &boxes,
                                               const std::vector<int> &class_indices,
                                               const std::vector<float> &scores,
                                               float iou_threshold)
{
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
    for (size_t i = 0; i < n; i++) indices[i] = i;
    std::sort(indices.begin(), indices.end(), [&](int a, int b) {
        return enriched[a].score > enriched[b].score;
    });

    std::vector<bool> suppressed(n, false);
    std::vector<detect_result_t> result;

    for (size_t i = 0; i < n; i++) {
        if (suppressed[indices[i]]) continue;
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

void NpuDetector::recover_score_info(const std::vector<float> &scores, int nboxes, int nclasses,
                                      std::vector<float> &hi_scores, std::vector<int> &class_indices)
{
    for (int box = 0; box < nboxes; box++) {
        int start = box * nclasses;
        auto max_it = std::max_element(scores.begin() + start + 1,
                                       scores.begin() + start + nclasses);
        hi_scores.push_back(*max_it);
        class_indices.push_back(std::distance(scores.begin() + start, max_it));
    }
}