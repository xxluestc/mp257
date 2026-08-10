/*
 * IMU 传感器驱动头文件。
 *
 * 通过 UART 接收 WitMotion 系列姿态传感器的二进制协议帧，
 * 解析加速度、陀螺仪、姿态角（roll/pitch/yaw）和磁力计数据，
 * 并以线程安全方式向主循环提供最新状态。
 */
#ifndef IMU_SENSOR_H
#define IMU_SENSOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "stm32wbaxx_hal.h"

/*
 * 逐字节接收结果位图：
 *   IMU_SENSOR_RX_FRAME             表示完成一帧（任意类型）；
 *   IMU_SENSOR_RX_ATTITUDE_FRAME    表示完成姿态帧；
 *   IMU_SENSOR_RX_GYRO_ACCEL_FRAME  表示完成六轴帧；
 *   IMU_SENSOR_RX_MAGNETOMETER_FRAME 表示完成磁力计帧。
 */
typedef enum
{
  IMU_SENSOR_RX_PENDING = 0,
  IMU_SENSOR_RX_FRAME = 0x01U,
  IMU_SENSOR_RX_ATTITUDE_FRAME = 0x02U,
  IMU_SENSOR_RX_GYRO_ACCEL_FRAME = 0x04U,
  IMU_SENSOR_RX_MAGNETOMETER_FRAME = 0x08U
} IMU_SensorRxResult_t;

/*
 * IMU 状态结构体。
 * valid 字段表示是否曾收到过对应类型的帧；
 * 具体数值为原始值，上层可根据传感器量程换算为物理单位。
 */
typedef struct
{
  uint8_t six_axis_valid;        /* 是否收到过六轴帧 */
  uint8_t attitude_valid;        /* 是否收到过姿态帧 */
  uint8_t magnetometer_valid;    /* 是否收到过磁力计帧 */
  int16_t ax;                    /* 加速度 X 原始值 */
  int16_t ay;                    /* 加速度 Y 原始值 */
  int16_t az;                    /* 加速度 Z 原始值 */
  int16_t gx;                    /* 陀螺仪 X 原始值 */
  int16_t gy;                    /* 陀螺仪 Y 原始值 */
  int16_t gz;                    /* 陀螺仪 Z 原始值（用于转弯检测） */
  int16_t mx;                    /* 磁力计 X 原始值 */
  int16_t my;                    /* 磁力计 Y 原始值 */
  int16_t mz;                    /* 磁力计 Z 原始值 */
  int16_t mag_temperature_cdeg;  /* 磁力计温度，0.01 摄氏度 */
  int16_t roll;                  /* 横滚角原始值 */
  int16_t pitch;                 /* 俯仰角原始值 */
  int16_t yaw;                   /* 航向角原始值（本工程用作 IMU 航向） */
  uint32_t six_axis_update_ms;   /* 六轴帧最新时间戳 */
  uint32_t attitude_update_ms;   /* 姿态帧最新时间戳 */
  uint32_t magnetometer_update_ms; /* 磁力计帧最新时间戳 */
} IMU_SensorState_t;

/* 初始化解析器状态和数据标志 */
void IMU_Sensor_Init(void);

/*
 * 逐字节喂入 IMU 串口数据。
 * 通常在 UART 接收完成中断中调用。
 * 返回位图表示是否完成帧以及完成帧的类型。
 */
IMU_SensorRxResult_t IMU_Sensor_ProcessByte(uint8_t byte);

/*
 * 主循环调用：输出已准备好的 IMU 数据日志。
 * output_uart 为 NULL 时只清标志不输出，用于关闭传感器日志时。
 */
void IMU_Sensor_ProcessReadyData(UART_HandleTypeDef *output_uart);

/*
 * 线程安全地获取最新 IMU 状态。
 * 调用前需传入非空指针。
 */
void IMU_Sensor_GetState(IMU_SensorState_t *state);

#ifdef __cplusplus
}
#endif

#endif /* IMU_SENSOR_H */
