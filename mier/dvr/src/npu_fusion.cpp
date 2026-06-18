/*
 * npu_fusion.cpp - NPU融合模块实现 (C++ → C API)
 *
 * 封装NpuDetector，添加道路用户过滤逻辑
 * 编译条件: NO_NPU_SUPPORT 未定义时编译NPU相关代码
 */

#include "npu_fusion.h"

#ifndef NO_NPU_SUPPORT

#include "npu_detector.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

/* 道路用户类别ID (COCO数据集) */
#define ROAD_USER_COUNT 6
static const int ROAD_USER_CLASSES[ROAD_USER_COUNT] = {1, 2, 3, 4, 6, 8};
static const char *ROAD_USER_NAMES[ROAD_USER_COUNT] = {
    "person", "bicycle", "car", "motorcycle", "bus", "truck"
};

struct npu_fusion_ctx {
    NpuDetector *detector;
    int          available;
    float        last_inference_time;
};

extern "C" {

npu_fusion_ctx_t *npu_fusion_create(const char *model_path,
                                     const char *labels_path,
                                     float confidence)
{
    if (!model_path || !model_path[0]) {
        printf("[NPU-FUSION] No model path, NPU disabled\n");
        return NULL;
    }

    npu_fusion_ctx_t *ctx = (npu_fusion_ctx_t *)calloc(1, sizeof(npu_fusion_ctx_t));
    if (!ctx) return NULL;

    ctx->detector = new NpuDetector();
    if (!ctx->detector) {
        free(ctx);
        return NULL;
    }

    NpuDetectorConfig cfg;
    cfg.model_path        = model_path;
    cfg.labels_path       = labels_path ? labels_path : "";
    cfg.confidence_thresh = confidence;
    cfg.iou_threshold     = 0.45f;
    cfg.num_threads       = 2;
    cfg.num_results       = 5;
    cfg.input_mean        = 127.5f;
    cfg.input_std         = 127.5f;

    if (!ctx->detector->Initialize(cfg)) {
        printf("[NPU-FUSION] Init failed, NPU disabled\n");
        delete ctx->detector;
        free(ctx);
        return NULL;
    }

    ctx->available = 1;
    printf("[NPU-FUSION] Initialized. Model: %s, confidence: %.2f\n",
           model_path, (double)confidence);
    return ctx;
}

void npu_fusion_destroy(npu_fusion_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->detector) {
        delete ctx->detector;
    }
    free(ctx);
}

int npu_fusion_available(npu_fusion_ctx_t *ctx)
{
    return ctx ? ctx->available : 0;
}

int npu_fusion_check_road_user(npu_fusion_ctx_t *ctx,
                                const uint8_t *rgb_data,
                                int width, int height)
{
    if (!ctx || !ctx->available || !rgb_data) return 0;

    std::vector<DetectedObject> objects = ctx->detector->RunInference(rgb_data, width, height);
    ctx->last_inference_time = ctx->detector->GetInferenceTime();

    int road_user_found = 0;
    printf("[NPU-FUSION] Inference: %.1fms, %zu objects\n",
           (double)ctx->last_inference_time, objects.size());

    for (size_t i = 0; i < objects.size(); i++) {
        int class_id = objects[i].class_id;
        float score  = objects[i].score;

        const char *class_name = "unknown";
        int is_road_user = 0;
        for (int j = 0; j < ROAD_USER_COUNT; j++) {
            if (class_id == ROAD_USER_CLASSES[j]) {
                is_road_user = 1;
                class_name = ROAD_USER_NAMES[j];
                break;
            }
        }

        printf("  [NPU] %s(id=%d) %.2f%% [%.2f,%.2f,%.2f,%.2f] %s\n",
               class_name, class_id, (double)(score * 100.0f),
               (double)objects[i].x0, (double)objects[i].y0,
               (double)objects[i].x1, (double)objects[i].y1,
               is_road_user ? "ROAD_USER" : "ignored");

        if (is_road_user) road_user_found = 1;
    }

    return road_user_found;
}

float npu_fusion_get_inference_time(npu_fusion_ctx_t *ctx)
{
    return ctx ? ctx->last_inference_time : 0.0f;
}

} /* extern "C" */

#else /* NO_NPU_SUPPORT */

/* 桩实现：NPU不可用时所有函数返回空/失败 */
#include <stdio.h>
#include <stdlib.h>

struct npu_fusion_ctx { int dummy; };

extern "C" {

npu_fusion_ctx_t *npu_fusion_create(const char *model_path,
                                     const char *labels_path,
                                     float confidence)
{
    (void)model_path; (void)labels_path; (void)confidence;
    printf("[NPU-FUSION] NPU not supported (NO_NPU_SUPPORT)\n");
    return NULL;
}

void npu_fusion_destroy(npu_fusion_ctx_t *ctx) { (void)ctx; }

int npu_fusion_available(npu_fusion_ctx_t *ctx) { (void)ctx; return 0; }

int npu_fusion_check_road_user(npu_fusion_ctx_t *ctx,
                                const uint8_t *rgb_data,
                                int width, int height)
{
    (void)ctx; (void)rgb_data; (void)width; (void)height;
    return 0;
}

float npu_fusion_get_inference_time(npu_fusion_ctx_t *ctx)
{
    (void)ctx;
    return 0.0f;
}

} /* extern "C" */

#endif /* NO_NPU_SUPPORT */