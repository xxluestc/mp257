/*
 * UART2 时分复用路由器头文件。
 *
 * 通过 GPIO 控制外部 TTL 扩展板，使单个 UART2 能够轮流与
 * GPS、IMU、语音模块通信，并提供语音预警播放的对外接口。
 */
#ifndef TTL_UART2_ROUTER_H
#define TTL_UART2_ROUTER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32wbaxx_hal.h"

/*
 * 传感器日志开关。
 * 在编译时定义：0 关闭 GPS/IMU 日志输出；1 打开。
 * 默认关闭，以节省 UART 带宽和 CPU 时间。
 */
#ifndef TTL_UART2_SENSOR_LOG_ENABLE
#define TTL_UART2_SENSOR_LOG_ENABLE 0U
#endif

/*
 * 初始化 UART2 时分复用路由器。
 * uart2:        复用的传感器/语音 UART 句柄；
 * output_uart:  日志输出 UART 句柄，可为 NULL。
 */
void TTL_UART2_Router_Init(UART_HandleTypeDef *uart2, UART_HandleTypeDef *output_uart);

/*
 * 开机问候语音：切换到语音通道，播放指定曲目。
 * 曲目号由 TTL_UART2_STARTUP_SONG_NUMBER 宏配置，
 * 默认 0，请根据语音模块里“你好”文件的实际编号修改。
 */
void TTL_UART2_PlayStartupGreeting(void);

/*
 * 启动 UART2 单字节中断接收。
 * 初始化后以及每次通道恢复后调用。
 */
void TTL_UART2_Router_StartRx(void);

/*
 * 主循环调用。
 * 处理待播报语音，并驱动 GPS/IMU 数据解析与日志输出。
 */
void TTL_UART2_Router_Process(void);

/*
 * UART 接收完成中断回调。
 * 在 HAL_UART_RxCpltCallback 中调用，由路由器按当前通道分发数据。
 */
void TTL_UART2_Router_RxCpltCallback(UART_HandleTypeDef *huart);

/*
 * UART 错误中断回调。
 * 在 HAL_UART_ErrorCallback 中调用，非语音通道时尝试恢复接收。
 */
void TTL_UART2_Router_ErrorCallback(UART_HandleTypeDef *huart);

#ifdef __cplusplus
}
#endif

#endif /* TTL_UART2_ROUTER_H */
