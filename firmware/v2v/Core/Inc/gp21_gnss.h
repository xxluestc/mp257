/*
 * GP21 GNSS 模块驱动头文件。
 *
 * 通过 UART 接收 NMEA-0183 语句，解析经纬度、速度、航向，
 * 并对主循环提供线程安全的 GNSS 状态读取接口。
 */
#ifndef GP21_GNSS_H
#define GP21_GNSS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "stm32wbaxx_hal.h"

/* 逐字节接收结果：PENDING 表示还在收，RMC_LINE 表示收到一条完整 RMC 行 */
typedef enum
{
  GP21_GNSS_RX_PENDING = 0,
  GP21_GNSS_RX_RMC_LINE
} GP21_GnssRxResult_t;

/* GNSS 定位状态结构体，所有字段在中断与主循环之间共享 */
typedef struct
{
  uint8_t valid;              /* 定位是否有效（A 标志、坐标合法） */
  int32_t latitude_1e7;       /* 纬度，单位 1e-7 度，北正南负 */
  int32_t longitude_1e7;      /* 经度，单位 1e-7 度，东正西负 */
  uint16_t speed_cms;         /* 速度，厘米/秒 */
  uint16_t heading_cdeg;      /* 航向，0.01 度，0~35999 */
  uint8_t pos_quality;        /* 定位质量（本工程复用为固定值 3） */
  uint32_t last_update_ms;    /* 最近一次有效更新的系统时间戳 */
} GP21_GnssState_t;

/* 初始化接收缓冲、状态机和滤波器 */
void GP21_Gnss_Init(void);

/*
 * 逐字节喂入 GNSS 串口数据。
 * 通常在 UART 接收完成中断中调用。
 * 返回 GP21_GNSS_RX_RMC_LINE 时表示已缓存一条完整 RMC 语句。
 */
GP21_GnssRxResult_t GP21_Gnss_ProcessByte(uint8_t byte);

/*
 * 主循环调用：取走已就绪的 RMC 行并解析。
 * output_uart 非 NULL 时输出解析日志；为 NULL 时只更新内部状态。
 */
void GP21_Gnss_ProcessReadyData(UART_HandleTypeDef *output_uart);

/*
 * 线程安全地获取最新 GNSS 状态。
 * 调用前需传入非空指针。
 */
void GP21_Gnss_GetState(GP21_GnssState_t *state);

#ifdef __cplusplus
}
#endif

#endif /* GP21_GNSS_H */
