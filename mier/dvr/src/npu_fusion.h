/*
 * npu_fusion.h - NPU融合模块 C API
 *
 * 封装C++ NpuDetector，提供C语言接口
 * 用于DVR引擎中验证雷达触发是否为道路用户
 *
 * 道路用户类别: person(1), bicycle(2), car(3), motorcycle(4), bus(6), truck(8)
 */

#ifndef NPU_FUSION_H
#define NPU_FUSION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 不透明句柄 */
typedef struct npu_fusion_ctx npu_fusion_ctx_t;

/*
 * 创建NPU融合上下文
 * @param model_path   .nb模型文件路径 (NULL表示不使用NPU)
 * @param labels_path  标签文件路径
 * @param confidence   置信度阈值 (0.0~1.0)
 * @return             上下文句柄, NULL表示失败或NPU不可用
 */
npu_fusion_ctx_t *npu_fusion_create(const char *model_path,
                                     const char *labels_path,
                                     float confidence);

/*
 * 销毁NPU融合上下文
 */
void npu_fusion_destroy(npu_fusion_ctx_t *ctx);

/*
 * 检查NPU是否可用
 */
int npu_fusion_available(npu_fusion_ctx_t *ctx);

/*
 * 对RGB帧运行NPU检测，判断是否包含道路用户
 * @param ctx        NPU上下文
 * @param rgb_data   RGB图像数据 (width*height*3)
 * @param width      图像宽度
 * @param height     图像高度
 * @return           1=检测到道路用户, 0=未检测到, -1=错误
 */
int npu_fusion_check_road_user(npu_fusion_ctx_t *ctx,
                                const uint8_t *rgb_data,
                                int width, int height);

/*
 * 获取最后一次推理耗时(毫秒)
 */
float npu_fusion_get_inference_time(npu_fusion_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* NPU_FUSION_H */