/*
 * npu_detector.hpp - 纯C++ NPU目标检测器（无Qt依赖）
 *
 * 基于 systemui_src/ssd_mobilenet_v2 的 staimpuwrapper 和 ssdmobilenetpp 剥离Qt而来
 * 使用 stai_mpu 库运行 SSD MobileNet V2 模型
 *
 * 编译条件: 需要开发板上的 stai_mpu 库和 .nb 模型文件
 *           VM上可通过 -DNO_NPU_SUPPORT 跳过编译
 */

#ifndef NPU_DETECTOR_HPP
#define NPU_DETECTOR_HPP

#ifndef NO_NPU_SUPPORT

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <sys/time.h>

/* 前向声明 stai_mpu 类型（避免在头文件中包含开发板SDK头文件） */
/* 实际编译时由 npu_detector.cpp 包含 stai_mpu_network.h */

/* 检测结果结构体 */
struct DetectedObject {
    int    class_id;      /* COCO类别ID */
    float  score;         /* 置信度 0.0~1.0 */
    float  x0, y0;        /* 归一化边界框左上角 (0.0~1.0) */
    float  x1, y1;        /* 归一化边界框右下角 (0.0~1.0) */
};

/* NPU检测器配置 */
struct NpuDetectorConfig {
    std::string model_path;       /* .nb模型文件路径 */
    std::string labels_path;      /* 标签文件路径 */
    float       confidence_thresh; /* 置信度阈值 (默认0.70) */
    float       iou_threshold;     /* NMS IOU阈值 (默认0.45) */
    int         num_threads;       /* 推理线程数 (默认2) */
    int         num_results;       /* 最大结果数 (默认5) */
    float       input_mean;        /* 输入归一化均值 (默认127.5) */
    float       input_std;         /* 输入归一化标准差 (默认127.5) */
};

/* NPU检测器类 */
class NpuDetector {
public:
    NpuDetector();
    ~NpuDetector();

    /* 初始化：加载模型和标签 */
    bool Initialize(const NpuDetectorConfig &config);

    /* 运行推理：输入RGB图像数据(256x256x3)，返回检测结果 */
    std::vector<DetectedObject> RunInference(const uint8_t *rgb_data,
                                             int width, int height);

    /* 获取模型输入尺寸 */
    int GetInputWidth()  const { return m_inputWidth; }
    int GetInputHeight() const { return m_inputHeight; }

    /* 获取推理耗时(ms) */
    float GetInferenceTime() const { return m_inferenceTime; }

    /* 读取标签文件 */
    bool ReadLabelsFile(const std::string &file_name);

    /* 获取标签列表 */
    const std::vector<std::string> &GetLabels() const { return mLabels; }

    /* 是否已初始化 */
    bool IsReady() const { return m_initialized; }

private:
    /* 内部实现细节（在cpp中用PIMPL隐藏stai_mpu依赖） */
    struct Impl;
    std::unique_ptr<Impl> m_pImpl;

    bool m_initialized;
    int  m_inputWidth;
    int  m_inputHeight;
    int  m_inputChannels;
    float m_inferenceTime;
    std::vector<std::string> mLabels;
    NpuDetectorConfig m_config;

    /* 辅助函数 */
    static double GetTimeMs(struct timeval t);
};

#endif /* NO_NPU_SUPPORT */
#endif /* NPU_DETECTOR_HPP */