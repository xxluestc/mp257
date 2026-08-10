/*
 * UART2 时分复用路由器。
 *
 * 通过 GPIO 控制外部 TTL 扩展板的通道切换，使单个 UART2 能够轮流与
 * GPS、IMU、语音模块通信。
 *
 * 调度策略：
 *   - 默认在 GPS 通道，收到完整 RMC 行后切换到 IMU；
 *   - IMU 通道收到足够帧（姿态+六轴）或达到最大帧数后切回 GPS；
 *   - 需要播报语音时临时切到语音通道，播完恢复。
 */
#include "ttl_uart2_router.h"

#include "app_ble.h"
#include "gp21_gnss.h"
#include "imu_sensor.h"

/* IMU 单周期最多接收帧数，防止卡死在 IMU 通道 */
#define TTL_UART2_IMU_MAX_FRAMES_PER_SLOT 6U
/* IMU 必须同时收到姿态帧和六轴帧才算一轮完整数据 */
#define TTL_UART2_IMU_REQUIRED_FRAME_MASK \
  (IMU_SENSOR_RX_ATTITUDE_FRAME | IMU_SENSOR_RX_GYRO_ACCEL_FRAME)
/* GPS/IMU 波特率 */
#define TTL_UART2_SENSOR_BAUDRATE 115200U
/* 语音模块波特率 */
#define TTL_UART2_VOICE_BAUDRATE 9600U
/*
 * 开机自检语音曲目编号。0 表示关闭。
 * 风险语音不能兼作问候语，否则每次复位都会产生一次假的左侧来车告警。
 * 可在 Keil 的 C/C++ 预处理宏中定义为专用问候曲目号，无需修改本文件。
 */
#ifndef TTL_UART2_STARTUP_SONG_NUMBER
#define TTL_UART2_STARTUP_SONG_NUMBER 0U
#endif

/* 外部 TTL 扩展板的三个通道 */
typedef enum
{
  TTL_UART2_GPS_CHANNEL = 0,
  TTL_UART2_IMU_CHANNEL = 1,
  TTL_UART2_VOICE_CHANNEL = 2
} TTL_UART2_Channel_t;

static UART_HandleTypeDef *sensor_uart;   /* 复用的 UART2 */
static UART_HandleTypeDef *output_uart;   /* 日志输出 UART */
static uint8_t uart2_rx_byte;             /* 单字节接收缓冲 */
static TTL_UART2_Channel_t active_channel;
static uint8_t imu_frames_in_slot;        /* 当前 IMU 周期已收帧数 */
static uint8_t imu_frame_mask_in_slot;    /* 当前 IMU 周期已收帧类型位图 */

/*
 * 切换 TTL 扩展板通道。
 * 用 GPIOA_PIN_1（A）和 GPIOA_PIN_2（B）编码：
 *   GPS=0:0, IMU=0:1, VOICE=1:0。
 * 切到 IMU 时重置该周期的帧计数和类型掩码。
 */
static void TTL_UART2_SelectChannel(TTL_UART2_Channel_t channel)
{
  GPIO_PinState select_a = (((uint8_t)channel & 0x01U) != 0U) ?
                            GPIO_PIN_SET : GPIO_PIN_RESET;
  GPIO_PinState select_b = (((uint8_t)channel & 0x02U) != 0U) ?
                            GPIO_PIN_SET : GPIO_PIN_RESET;

  if (channel == TTL_UART2_IMU_CHANNEL)
  {
    imu_frames_in_slot = 0U;
    imu_frame_mask_in_slot = 0U;
  }

  /* The TTL expansion board uses B:A. GPS is 0:0, IMU is 0:1, voice is 1:0. */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2, select_b);
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, select_a);
  active_channel = channel;
}

/* 动态修改 UART2 波特率，语音模块与传感器模块波特率不同。
 *
 * 与原始代码保持一致：直接调用 HAL_UART_Init 重新加载 Init 结构体。
 * 由于 huart 已经初始化过（gState != RESET），HAL 不会再次调用 MspInit，
 * 只会更新波特率寄存器并检查 UART 空闲状态，避免 GPIO 被重置。
 */
static HAL_StatusTypeDef TTL_UART2_SetBaudRate(uint32_t baud_rate)
{
  if (sensor_uart == NULL)
  {
    return HAL_ERROR;
  }

  sensor_uart->Init.BaudRate = baud_rate;
  return HAL_UART_Init(sensor_uart);
}

/* 清除 UART 帧错误、噪声、过载等标志，避免切换通道后误中断 */
static void TTL_UART2_ClearRxErrors(void)
{
  __HAL_UART_CLEAR_FLAG(sensor_uart, UART_CLEAR_PEF | UART_CLEAR_FEF |
                                     UART_CLEAR_NEF | UART_CLEAR_OREF);
}

/*
 * 以下 Append* 工具函数用于无 printf 环境构造日志字符串。
 */

static uint16_t TTL_UART2_AppendString(char *dst, uint16_t pos, uint16_t size, const char *src)
{
  while ((src != NULL) && (*src != '\0') && ((pos + 1U) < size))
  {
    dst[pos++] = *src++;
    dst[pos] = '\0';
  }

  return pos;
}

static uint16_t TTL_UART2_AppendChar(char *dst, uint16_t pos, uint16_t size, char c)
{
  if ((pos + 1U) < size)
  {
    dst[pos++] = c;
    dst[pos] = '\0';
  }

  return pos;
}

static uint16_t TTL_UART2_AppendUInt(char *dst, uint16_t pos, uint16_t size, uint32_t value)
{
  char tmp[10];
  uint8_t len = 0U;

  do
  {
    tmp[len++] = (char)('0' + (value % 10U));
    value /= 10U;
  } while ((value > 0U) && (len < sizeof(tmp)));

  while ((len > 0U) && ((pos + 1U) < size))
  {
    dst[pos++] = tmp[--len];
    dst[pos] = '\0';
  }

  return pos;
}

static uint16_t TTL_UART2_AppendInt(char *dst, uint16_t pos, uint16_t size, int32_t value)
{
  if ((value < 0) && ((pos + 1U) < size))
  {
    dst[pos++] = '-';
    dst[pos] = '\0';
    value = -value;
  }

  return TTL_UART2_AppendUInt(dst, pos, size, (uint32_t)value);
}

/* 把语音枚举转换为可读的日志名称 */
static const char *TTL_UART2_VoicePromptName(APP_BLE_VoicePrompt_t prompt)
{
  switch (prompt)
  {
    case APP_BLE_VOICE_LEFT_RISK:
      return "VOICE_LEFT_RISK";
    case APP_BLE_VOICE_RIGHT_RISK:
      return "VOICE_RIGHT_RISK";
    case APP_BLE_VOICE_LEFT_FRONT_RISK:
      return "VOICE_LEFT_FRONT_RISK";
    case APP_BLE_VOICE_RIGHT_FRONT_RISK:
      return "VOICE_RIGHT_FRONT_RISK";
    case APP_BLE_VOICE_GPS_WEAK:
      return "VOICE_GPS_WEAK";
    default:
      return "VOICE_NONE";
  }
}

/* 语音枚举值直接对应 MP3 模块的曲目编号 */
static uint16_t TTL_UART2_VoiceSongNumber(APP_BLE_VoicePrompt_t prompt)
{
  if ((prompt >= APP_BLE_VOICE_LEFT_RISK) && (prompt <= APP_BLE_VOICE_GPS_WEAK))
  {
    return (uint16_t)prompt;
  }

  return 0U;
}

/*
 * 构造 MP3 播放命令帧。
 * 格式：FE 09 FF FF 07 song_h song_l checksum BE
 * checksum 为前 7 字节之和的低 8 位。
 */
static void TTL_UART2_BuildVoiceCommand(uint16_t song_number, uint8_t *cmd)
{
  uint16_t checksum = 0U;

  cmd[0] = 0xFEU;
  cmd[1] = 0x09U;
  cmd[2] = 0xFFU;
  cmd[3] = 0xFFU;
  cmd[4] = 0x07U;
  cmd[5] = (uint8_t)(song_number >> 8);
  cmd[6] = (uint8_t)(song_number & 0xFFU);
  for (uint8_t i = 0U; i < 7U; i++)
  {
    checksum = (uint16_t)(checksum + cmd[i]);
  }
  cmd[7] = (uint8_t)(checksum & 0xFFU);
  cmd[8] = 0xBEU;
}

/* 输出语音播放日志，包含曲目号、对端 ID、相对位置等信息 */
static void TTL_UART2_LogVoicePlayback(const APP_BLE_VoiceWarning_t *warning,
                                       uint16_t song_number)
{
  char msg[180] = {0};
  uint16_t len = 0U;

  if ((output_uart == NULL) || (warning == NULL))
  {
    return;
  }

  len = TTL_UART2_AppendString(msg, len, sizeof(msg), "[VOICE] play ");
  len = TTL_UART2_AppendString(msg, len, sizeof(msg),
                               TTL_UART2_VoicePromptName(warning->prompt));
  len = TTL_UART2_AppendString(msg, len, sizeof(msg), " file=");
  if (song_number < 10000U)
  {
    len = TTL_UART2_AppendString(msg, len, sizeof(msg), "0");
  }
  if (song_number < 1000U)
  {
    len = TTL_UART2_AppendString(msg, len, sizeof(msg), "0");
  }
  if (song_number < 100U)
  {
    len = TTL_UART2_AppendString(msg, len, sizeof(msg), "0");
  }
  if (song_number < 10U)
  {
    len = TTL_UART2_AppendString(msg, len, sizeof(msg), "0");
  }
  len = TTL_UART2_AppendUInt(msg, len, sizeof(msg), song_number);
  len = TTL_UART2_AppendString(msg, len, sizeof(msg), " device=");
  len = TTL_UART2_AppendUInt(msg, len, sizeof(msg), warning->device_id);
  len = TTL_UART2_AppendString(msg, len, sizeof(msg), " distance_cm=");
  len = TTL_UART2_AppendUInt(msg, len, sizeof(msg), warning->distance_cm);
  len = TTL_UART2_AppendString(msg, len, sizeof(msg), " y_front_cm=");
  len = TTL_UART2_AppendInt(msg, len, sizeof(msg), warning->y_front_cm);
  len = TTL_UART2_AppendString(msg, len, sizeof(msg), " x_right_cm=");
  len = TTL_UART2_AppendInt(msg, len, sizeof(msg), warning->x_right_cm);
  len = TTL_UART2_AppendString(msg, len, sizeof(msg), "\r\n");

  (void)HAL_UART_Transmit(output_uart, (uint8_t *)msg, len, 100U);
}

/* 把 4bit 数值转换为十六进制字符 */
static char TTL_UART2_NibbleToHex(uint8_t nibble)
{
  if (nibble <= 9U)
  {
    return (char)('0' + nibble);
  }

  return (char)('A' + (nibble - 10U));
}

/* 输出实际发送给 MP3 模块的十六进制命令帧，用于硬件联调 */
static void TTL_UART2_LogVoiceCommandHex(const uint8_t *cmd, uint16_t cmd_len)
{
  char msg[64] = {0};
  uint16_t len = 0U;

  if ((output_uart == NULL) || (cmd == NULL) || (cmd_len == 0U))
  {
    return;
  }

  len = TTL_UART2_AppendString(msg, len, sizeof(msg), "[VOICE] tx cmd:");
  for (uint16_t i = 0U; i < cmd_len; i++)
  {
    len = TTL_UART2_AppendString(msg, len, sizeof(msg), " ");
    len = TTL_UART2_AppendChar(msg, len, sizeof(msg), TTL_UART2_NibbleToHex((uint8_t)(cmd[i] >> 4U)));
    len = TTL_UART2_AppendChar(msg, len, sizeof(msg), TTL_UART2_NibbleToHex((uint8_t)(cmd[i] & 0x0FU)));
  }
  len = TTL_UART2_AppendString(msg, len, sizeof(msg), "\r\n");

  (void)HAL_UART_Transmit(output_uart, (uint8_t *)msg, len, 100U);
}

/*
 * 播放语音预警。
 * 流程：切到语音通道 -> 改波特率 -> 发播放命令 -> 改回传感器波特率 -> 恢复通道。
 * 注意：此过程会短暂阻塞（用 HAL_Delay 给模块切换留时间）。
 */
static void TTL_UART2_PlayVoiceWarning(const APP_BLE_VoiceWarning_t *warning)
{
  uint8_t play_cmd[9];
  uint16_t song_number;
  TTL_UART2_Channel_t restore_channel;

  if ((sensor_uart == NULL) || (warning == NULL))
  {
    return;
  }

  song_number = TTL_UART2_VoiceSongNumber(warning->prompt);
  if (song_number == 0U)
  {
    return;
  }
  TTL_UART2_BuildVoiceCommand(song_number, play_cmd);

  restore_channel = active_channel;
  (void)HAL_UART_AbortReceive(sensor_uart);
  TTL_UART2_ClearRxErrors();

  TTL_UART2_SelectChannel(TTL_UART2_VOICE_CHANNEL);
  /* 给 TTL 扩展板和语音模块留足稳定时间 */
  HAL_Delay(50U);

  if (TTL_UART2_SetBaudRate(TTL_UART2_VOICE_BAUDRATE) == HAL_OK)
  {
    TTL_UART2_LogVoicePlayback(warning, song_number);
    TTL_UART2_LogVoiceCommandHex(play_cmd, (uint16_t)sizeof(play_cmd));
    if (HAL_UART_Transmit(sensor_uart, play_cmd, (uint16_t)sizeof(play_cmd), 100U) == HAL_OK)
    {
      /* 命令发完后等待模块开始播放 */
      HAL_Delay(50U);
    }
    else
    {
      TTL_UART2_LogVoicePlayback(warning, 0U);
    }
  }

  (void)TTL_UART2_SetBaudRate(TTL_UART2_SENSOR_BAUDRATE);
  TTL_UART2_ClearRxErrors();
  TTL_UART2_SelectChannel(restore_channel);
  TTL_UART2_Router_StartRx();
}

/*
 * 开机问候：切换到语音通道，播放 TTL_UART2_STARTUP_SONG_NUMBER 指定的曲目。
 * 在系统初始化完成后调用，用于验证语音模块通路是否正常。
 *
 * 注意：有些 MP3 模块上电后需要数百毫秒初始化 SD 卡和解码器，
 * 如果刚复位就发命令会被忽略。因此在切通道前额外等待 500ms。
 */
void TTL_UART2_PlayStartupGreeting(void)
{
  uint8_t play_cmd[9];
  TTL_UART2_Channel_t restore_channel;

  if ((sensor_uart == NULL) || (TTL_UART2_STARTUP_SONG_NUMBER == 0U))
  {
    return;
  }

  /* 等待语音模块完成上电初始化，否则早期命令可能被忽略 */
  HAL_Delay(500U);

  TTL_UART2_BuildVoiceCommand(TTL_UART2_STARTUP_SONG_NUMBER, play_cmd);

  restore_channel = active_channel;
  (void)HAL_UART_AbortReceive(sensor_uart);
  TTL_UART2_ClearRxErrors();

  TTL_UART2_SelectChannel(TTL_UART2_VOICE_CHANNEL);
  /* 给 TTL 扩展板继电器/模拟开关留足稳定时间 */
  HAL_Delay(100U);

  if (TTL_UART2_SetBaudRate(TTL_UART2_VOICE_BAUDRATE) == HAL_OK)
  {
    TTL_UART2_LogVoiceCommandHex(play_cmd, (uint16_t)sizeof(play_cmd));
    if (HAL_UART_Transmit(sensor_uart, play_cmd, (uint16_t)sizeof(play_cmd), 100U) == HAL_OK)
    {
      /* 命令发完后等待模块开始播放 */
      HAL_Delay(100U);
    }
  }

  (void)TTL_UART2_SetBaudRate(TTL_UART2_SENSOR_BAUDRATE);
  TTL_UART2_ClearRxErrors();
  TTL_UART2_SelectChannel(restore_channel);
  TTL_UART2_Router_StartRx();
}

/* 初始化路由器，保存 UART 句柄，初始化 GPS/IMU 驱动，默认选择 GPS 通道 */
void TTL_UART2_Router_Init(UART_HandleTypeDef *uart2, UART_HandleTypeDef *uart_output)
{
  sensor_uart = uart2;
  output_uart = uart_output;
  GP21_Gnss_Init();
  IMU_Sensor_Init();
  TTL_UART2_SelectChannel(TTL_UART2_GPS_CHANNEL);
}

/* 启动 UART2 单字节中断接收 */
void TTL_UART2_Router_StartRx(void)
{
  if (sensor_uart != NULL)
  {
    (void)HAL_UART_Receive_IT(sensor_uart, &uart2_rx_byte, 1U);
  }
}

/*
 * 主循环调用。
 * 1) 取待播报语音并播放；
 * 2) 处理已就绪的 GPS/IMU 数据；
 *    当 TTL_UART2_SENSOR_LOG_ENABLE=1 时输出到 output_uart，否则只清标志。
 */
void TTL_UART2_Router_Process(void)
{
  APP_BLE_VoiceWarning_t voice_warning;

  if (APP_BLE_TakeVoiceWarning(&voice_warning) != 0U)
  {
    TTL_UART2_PlayVoiceWarning(&voice_warning);
  }

#if (TTL_UART2_SENSOR_LOG_ENABLE != 0U)
  GP21_Gnss_ProcessReadyData(output_uart);
  IMU_Sensor_ProcessReadyData(output_uart);
#else
  GP21_Gnss_ProcessReadyData(NULL);
  IMU_Sensor_ProcessReadyData(NULL);
#endif
}

/*
 * UART 接收完成中断回调。
 * 根据当前通道把字节交给 GPS 或 IMU 解析器，并按规则切换通道。
 */
void TTL_UART2_Router_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if ((sensor_uart == NULL) || (huart != sensor_uart))
  {
    return;
  }

  if (active_channel == TTL_UART2_GPS_CHANNEL)
  {
    if (GP21_Gnss_ProcessByte(uart2_rx_byte) == GP21_GNSS_RX_RMC_LINE)
    {
      TTL_UART2_SelectChannel(TTL_UART2_IMU_CHANNEL);
    }
  }
  else if (active_channel == TTL_UART2_IMU_CHANNEL)
  {
    IMU_SensorRxResult_t imu_result = IMU_Sensor_ProcessByte(uart2_rx_byte);

    if ((imu_result & IMU_SENSOR_RX_FRAME) != 0U)
    {
      if (imu_frames_in_slot < 255U)
      {
        imu_frames_in_slot++;
      }
      imu_frame_mask_in_slot |= (uint8_t)(imu_result &
                                          (IMU_SENSOR_RX_ATTITUDE_FRAME |
                                           IMU_SENSOR_RX_GYRO_ACCEL_FRAME |
                                           IMU_SENSOR_RX_MAGNETOMETER_FRAME));
      if (((imu_frame_mask_in_slot & TTL_UART2_IMU_REQUIRED_FRAME_MASK) ==
           TTL_UART2_IMU_REQUIRED_FRAME_MASK) ||
          (imu_frames_in_slot >= TTL_UART2_IMU_MAX_FRAMES_PER_SLOT))
      {
        TTL_UART2_SelectChannel(TTL_UART2_GPS_CHANNEL);
      }
    }
  }

  if (active_channel != TTL_UART2_VOICE_CHANNEL)
  {
    TTL_UART2_Router_StartRx();
  }
}

/* UART 错误中断回调：在非语音通道时重新启动接收 */
void TTL_UART2_Router_ErrorCallback(UART_HandleTypeDef *huart)
{
  if ((sensor_uart != NULL) && (huart == sensor_uart) &&
      (active_channel != TTL_UART2_VOICE_CHANNEL))
  {
    TTL_UART2_Router_StartRx();
  }
}
