/*
 * IMU 传感器驱动。
 *
 * 通过 UART 接收 WitMotion 系列姿态传感器的二进制协议帧，
 * 解析出加速度、陀螺仪、姿态角（roll/pitch/yaw）和磁力计数据。
 * 上层通过 IMU_Sensor_GetState() 以线程安全方式读取最新状态。
 */
#include "imu_sensor.h"

/* 帧头：协议规定以 0x55 开头，连续出现两次 */
#define IMU_FRAME_HEAD 0x55U
/* 姿态帧 ID：包含 roll / pitch / yaw */
#define IMU_FRAME_ID_ATTITUDE 0x01U
/* 六轴帧 ID：包含加速度 ax/ay/az 和陀螺仪 gx/gy/gz */
#define IMU_FRAME_ID_GYRO_ACCEL 0x03U
/* 磁力计帧 ID：包含 mx/my/mz 和温度 */
#define IMU_FRAME_ID_MAGNETOMETER 0x04U
/* 数据域最大长度，防止异常长度越界 */
#define IMU_FRAME_DATA_MAX_SIZE 28U

/*
 * 接收状态机：
 *   WAIT_HEAD_L -> WAIT_HEAD_H -> WAIT_ID -> WAIT_LEN -> WAIT_DATA -> WAIT_SUM
 * 校验通过后将数据写入对应缓冲区。
 */
typedef enum
{
  IMU_WAIT_HEAD_L = 0,
  IMU_WAIT_HEAD_H,
  IMU_WAIT_ID,
  IMU_WAIT_LEN,
  IMU_WAIT_DATA,
  IMU_WAIT_SUM
} IMU_ParserState_t;

/* 六轴原始数据：加速度（待上层换算为 g）和陀螺仪（待上层换算为 deg/s） */
typedef struct
{
  int16_t ax;
  int16_t ay;
  int16_t az;
  int16_t gx;
  int16_t gy;
  int16_t gz;
} IMU_SixAxisRaw_t;

/* 磁力计原始数据：三轴磁场和温度 */
typedef struct
{
  int16_t mx;
  int16_t my;
  int16_t mz;
  int16_t temperature_cdeg;
} IMU_MagnetometerRaw_t;

/* 接收解析器上下文 */
typedef struct
{
  uint8_t id;            /* 帧 ID */
  uint8_t len;           /* 数据域长度 */
  uint8_t index;         /* 当前已接收数据字节数 */
  uint8_t checksum;      /* 累加和校验（从帧头开始累加） */
  uint8_t data[IMU_FRAME_DATA_MAX_SIZE];
  IMU_ParserState_t state;
} IMU_Parser_t;

static IMU_Parser_t parser;

/*
 * 以下 volatile 变量在中断接收与主循环读取之间共享，
 * 读取时需要关中断保证原子性。
 */
static volatile IMU_SixAxisRaw_t ready_six_axis;       /* 最新六轴数据 */
static volatile IMU_MagnetometerRaw_t ready_magnetometer; /* 最新磁力计数据 */
static volatile int16_t ready_roll;                  /* 横滚角原始值 */
static volatile int16_t ready_pitch;                 /* 俯仰角原始值 */
static volatile int16_t ready_yaw;                   /* 航向角原始值 */
static volatile uint32_t ready_six_axis_tick;        /* 六轴数据时间戳 */
static volatile uint32_t ready_attitude_tick;        /* 姿态数据时间戳 */
static volatile uint32_t ready_magnetometer_tick;    /* 磁力计数据时间戳 */
static volatile uint8_t six_axis_ready;              /* 本周期是否有新六轴帧 */
static volatile uint8_t attitude_ready;              /* 本周期是否有新姿态帧 */
static volatile uint8_t magnetometer_ready;          /* 本周期是否有新磁力计帧 */
static volatile uint8_t six_axis_seen;               /* 是否曾经收到过六轴帧 */
static volatile uint8_t attitude_seen;               /* 是否曾经收到过姿态帧 */
static volatile uint8_t magnetometer_seen;           /* 是否曾经收到过磁力计帧 */

/* 小端读取 int16，IMU 协议数据域采用低字节在前 */
static int16_t IMU_ReadInt16(const uint8_t *data)
{
  return (int16_t)(((uint16_t)data[1] << 8U) | data[0]);
}

/*
 * 一帧校验通过后将数据分类保存。
 * 返回 IMU_SensorRxResult_t 位图，告诉调用者收到哪些类型的帧。
 */
static IMU_SensorRxResult_t IMU_StoreReadyFrame(void)
{
  IMU_SensorRxResult_t result = IMU_SENSOR_RX_FRAME;

  if ((parser.id == IMU_FRAME_ID_GYRO_ACCEL) && (parser.len >= 12U))
  {
    ready_six_axis.ax = IMU_ReadInt16(&parser.data[0]);
    ready_six_axis.ay = IMU_ReadInt16(&parser.data[2]);
    ready_six_axis.az = IMU_ReadInt16(&parser.data[4]);
    ready_six_axis.gx = IMU_ReadInt16(&parser.data[6]);
    ready_six_axis.gy = IMU_ReadInt16(&parser.data[8]);
    ready_six_axis.gz = IMU_ReadInt16(&parser.data[10]);
    ready_six_axis_tick = HAL_GetTick();
    six_axis_ready = 1U;
    six_axis_seen = 1U;
    result = (IMU_SensorRxResult_t)(result | IMU_SENSOR_RX_GYRO_ACCEL_FRAME);
  }
  else if ((parser.id == IMU_FRAME_ID_ATTITUDE) && (parser.len >= 6U))
  {
    ready_roll = IMU_ReadInt16(&parser.data[0]);
    ready_pitch = IMU_ReadInt16(&parser.data[2]);
    ready_yaw = IMU_ReadInt16(&parser.data[4]);
    ready_attitude_tick = HAL_GetTick();
    attitude_ready = 1U;
    attitude_seen = 1U;
    result = (IMU_SensorRxResult_t)(result | IMU_SENSOR_RX_ATTITUDE_FRAME);
  }
  else if ((parser.id == IMU_FRAME_ID_MAGNETOMETER) && (parser.len >= 8U))
  {
    ready_magnetometer.mx = IMU_ReadInt16(&parser.data[0]);
    ready_magnetometer.my = IMU_ReadInt16(&parser.data[2]);
    ready_magnetometer.mz = IMU_ReadInt16(&parser.data[4]);
    ready_magnetometer.temperature_cdeg = IMU_ReadInt16(&parser.data[6]);
    ready_magnetometer_tick = HAL_GetTick();
    magnetometer_ready = 1U;
    magnetometer_seen = 1U;
    result = (IMU_SensorRxResult_t)(result | IMU_SENSOR_RX_MAGNETOMETER_FRAME);
  }

  return result;
}

/*
 * 失步或校验失败时复位解析器。
 * 如果当前字节正好是 0x55，则直接进入等待第二个帧头的状态，
 * 避免漏掉紧挨着的一帧开头。
 */
static void IMU_RestartFromByte(uint8_t byte)
{
  parser.index = 0U;
  parser.len = 0U;
  if (byte == IMU_FRAME_HEAD)
  {
    parser.checksum = byte;
    parser.state = IMU_WAIT_HEAD_H;
  }
  else
  {
    parser.checksum = 0U;
    parser.state = IMU_WAIT_HEAD_L;
  }
}

/*
 * 以下 Append* 工具函数用于在禁用了 printf 的嵌入式环境中，
 * 把整数/字符串安全地写入定长缓冲区，供调试日志输出。
 */

static uint16_t IMU_AppendChar(char *dst, uint16_t pos, uint16_t size, char c)
{
  if ((pos + 1U) < size)
  {
    dst[pos++] = c;
    dst[pos] = '\0';
  }

  return pos;
}

static uint16_t IMU_AppendString(char *dst, uint16_t pos, uint16_t size, const char *src)
{
  while ((src != NULL) && (*src != '\0'))
  {
    pos = IMU_AppendChar(dst, pos, size, *src);
    src++;
  }

  return pos;
}

static uint16_t IMU_AppendUInt(char *dst, uint16_t pos, uint16_t size, uint32_t value)
{
  char tmp[10];
  uint8_t len = 0U;

  do
  {
    tmp[len++] = (char)('0' + (value % 10U));
    value /= 10U;
  } while ((value > 0U) && (len < sizeof(tmp)));

  while (len > 0U)
  {
    pos = IMU_AppendChar(dst, pos, size, tmp[--len]);
  }

  return pos;
}

static uint16_t IMU_AppendInt(char *dst, uint16_t pos, uint16_t size, int32_t value)
{
  if (value < 0)
  {
    pos = IMU_AppendChar(dst, pos, size, '-');
    value = -value;
  }

  return IMU_AppendUInt(dst, pos, size, (uint32_t)value);
}

/*
 * 把 IMU 原始角度（有符号 16bit，满量程 32768 对应 180 度）
 * 转换为“度.两位小数”字符串，用于日志。
 */
static uint16_t IMU_AppendAngle(char *dst, uint16_t pos, uint16_t size, int16_t raw)
{
  int32_t centidegree;
  uint32_t abs_value;
  uint32_t fraction;

  centidegree = ((int32_t)raw * 18000) / 32768;
  if (centidegree < 0)
  {
    pos = IMU_AppendChar(dst, pos, size, '-');
    abs_value = (uint32_t)(-centidegree);
  }
  else
  {
    abs_value = (uint32_t)centidegree;
  }

  fraction = abs_value % 100U;
  pos = IMU_AppendUInt(dst, pos, size, abs_value / 100U);
  pos = IMU_AppendChar(dst, pos, size, '.');
  pos = IMU_AppendChar(dst, pos, size, (char)('0' + (fraction / 10U)));
  pos = IMU_AppendChar(dst, pos, size, (char)('0' + (fraction % 10U)));

  return pos;
}

/*
 * 把原始值按“整数.两位小数”格式输出。
 * 例如温度原始值 2543 输出为 25.43。
 */
static uint16_t IMU_AppendCentiValue(char *dst, uint16_t pos, uint16_t size, int16_t raw)
{
  int32_t value = raw;
  uint32_t abs_value;
  uint32_t fraction;

  if (value < 0)
  {
    pos = IMU_AppendChar(dst, pos, size, '-');
    abs_value = (uint32_t)(-value);
  }
  else
  {
    abs_value = (uint32_t)value;
  }

  fraction = abs_value % 100U;
  pos = IMU_AppendUInt(dst, pos, size, abs_value / 100U);
  pos = IMU_AppendChar(dst, pos, size, '.');
  pos = IMU_AppendChar(dst, pos, size, (char)('0' + (fraction / 10U)));
  pos = IMU_AppendChar(dst, pos, size, (char)('0' + (fraction % 10U)));

  return pos;
}

/* 输出六轴原始数据日志 */
static void IMU_OutputSixAxis(UART_HandleTypeDef *output_uart, const IMU_SixAxisRaw_t *data)
{
  char msg[112] = {0};
  uint16_t len = 0U;

  if (output_uart == NULL)
  {
    return;
  }

  len = IMU_AppendString(msg, len, sizeof(msg), "[IMU] six_axis raw ax=");
  len = IMU_AppendInt(msg, len, sizeof(msg), data->ax);
  len = IMU_AppendString(msg, len, sizeof(msg), " ay=");
  len = IMU_AppendInt(msg, len, sizeof(msg), data->ay);
  len = IMU_AppendString(msg, len, sizeof(msg), " az=");
  len = IMU_AppendInt(msg, len, sizeof(msg), data->az);
  len = IMU_AppendString(msg, len, sizeof(msg), " gx=");
  len = IMU_AppendInt(msg, len, sizeof(msg), data->gx);
  len = IMU_AppendString(msg, len, sizeof(msg), " gy=");
  len = IMU_AppendInt(msg, len, sizeof(msg), data->gy);
  len = IMU_AppendString(msg, len, sizeof(msg), " gz=");
  len = IMU_AppendInt(msg, len, sizeof(msg), data->gz);
  len = IMU_AppendString(msg, len, sizeof(msg), "\r\n");

  (void)HAL_UART_Transmit(output_uart, (uint8_t *)msg, len, 50U);
}

/* 输出磁力计原始数据日志 */
static void IMU_OutputMagnetometer(UART_HandleTypeDef *output_uart, const IMU_MagnetometerRaw_t *data)
{
  char msg[96] = {0};
  uint16_t len = 0U;

  if (output_uart == NULL)
  {
    return;
  }

  len = IMU_AppendString(msg, len, sizeof(msg), "[IMU] mag raw mx=");
  len = IMU_AppendInt(msg, len, sizeof(msg), data->mx);
  len = IMU_AppendString(msg, len, sizeof(msg), " my=");
  len = IMU_AppendInt(msg, len, sizeof(msg), data->my);
  len = IMU_AppendString(msg, len, sizeof(msg), " mz=");
  len = IMU_AppendInt(msg, len, sizeof(msg), data->mz);
  len = IMU_AppendString(msg, len, sizeof(msg), " temp=");
  len = IMU_AppendCentiValue(msg, len, sizeof(msg), data->temperature_cdeg);
  len = IMU_AppendString(msg, len, sizeof(msg), "C\r\n");

  (void)HAL_UART_Transmit(output_uart, (uint8_t *)msg, len, 50U);
}

/* 输出姿态角日志 */
static void IMU_OutputAttitude(UART_HandleTypeDef *output_uart, int16_t roll, int16_t pitch, int16_t yaw)
{
  char msg[96] = {0};
  uint16_t len = 0U;

  if (output_uart == NULL)
  {
    return;
  }

  len = IMU_AppendString(msg, len, sizeof(msg), "[IMU] attitude roll=");
  len = IMU_AppendAngle(msg, len, sizeof(msg), roll);
  len = IMU_AppendString(msg, len, sizeof(msg), " pitch=");
  len = IMU_AppendAngle(msg, len, sizeof(msg), pitch);
  len = IMU_AppendString(msg, len, sizeof(msg), " yaw=");
  len = IMU_AppendAngle(msg, len, sizeof(msg), yaw);
  len = IMU_AppendString(msg, len, sizeof(msg), " deg\r\n");

  (void)HAL_UART_Transmit(output_uart, (uint8_t *)msg, len, 50U);
}

/* 初始化解析器状态和所有数据标志 */
void IMU_Sensor_Init(void)
{
  parser.id = 0U;
  parser.len = 0U;
  parser.index = 0U;
  parser.checksum = 0U;
  parser.state = IMU_WAIT_HEAD_L;
  six_axis_ready = 0U;
  attitude_ready = 0U;
  magnetometer_ready = 0U;
  six_axis_seen = 0U;
  attitude_seen = 0U;
  magnetometer_seen = 0U;
  ready_six_axis_tick = 0U;
  ready_attitude_tick = 0U;
  ready_magnetometer_tick = 0U;
}

/*
 * 逐字节状态机解析 IMU 二进制帧。
 * 通常在 UART 接收完成中断中调用。
 * 返回位图，表示本字节是否完成一帧以及完成的是哪种帧。
 */
IMU_SensorRxResult_t IMU_Sensor_ProcessByte(uint8_t byte)
{
  IMU_SensorRxResult_t result = IMU_SENSOR_RX_PENDING;

  switch (parser.state)
  {
    case IMU_WAIT_HEAD_L:
      if (byte == IMU_FRAME_HEAD)
      {
        parser.checksum = byte;
        parser.state = IMU_WAIT_HEAD_H;
      }
      break;

    case IMU_WAIT_HEAD_H:
      if (byte == IMU_FRAME_HEAD)
      {
        parser.checksum += byte;
        parser.state = IMU_WAIT_ID;
      }
      else
      {
        IMU_RestartFromByte(byte);
      }
      break;

    case IMU_WAIT_ID:
      parser.id = byte;
      parser.checksum += byte;
      parser.state = IMU_WAIT_LEN;
      break;

    case IMU_WAIT_LEN:
      if (byte <= IMU_FRAME_DATA_MAX_SIZE)
      {
        parser.len = byte;
        parser.index = 0U;
        parser.checksum += byte;
        parser.state = (byte == 0U) ? IMU_WAIT_SUM : IMU_WAIT_DATA;
      }
      else
      {
        IMU_RestartFromByte(byte);
      }
      break;

    case IMU_WAIT_DATA:
      parser.data[parser.index++] = byte;
      parser.checksum += byte;
      if (parser.index >= parser.len)
      {
        parser.state = IMU_WAIT_SUM;
      }
      break;

    case IMU_WAIT_SUM:
      if (byte == parser.checksum)
      {
        result = IMU_StoreReadyFrame();
        IMU_RestartFromByte(0U);
      }
      else
      {
        IMU_RestartFromByte(byte);
      }
      break;

    default:
      IMU_RestartFromByte(byte);
      break;
  }

  return result;
}

/*
 * 主循环调用：把中断中准备好的数据取走并输出日志。
 * output_uart 为 NULL 时只清标志不输出，用于传感器日志关闭时。
 */
void IMU_Sensor_ProcessReadyData(UART_HandleTypeDef *output_uart)
{
  IMU_SixAxisRaw_t six_axis;
  IMU_MagnetometerRaw_t magnetometer;
  int16_t roll;
  int16_t pitch;
  int16_t yaw;
  uint8_t copy_six_axis;
  uint8_t copy_attitude;
  uint8_t copy_magnetometer;

  __disable_irq();
  copy_six_axis = six_axis_ready;
  copy_attitude = attitude_ready;
  copy_magnetometer = magnetometer_ready;
  six_axis.ax = ready_six_axis.ax;
  six_axis.ay = ready_six_axis.ay;
  six_axis.az = ready_six_axis.az;
  six_axis.gx = ready_six_axis.gx;
  six_axis.gy = ready_six_axis.gy;
  six_axis.gz = ready_six_axis.gz;
  magnetometer.mx = ready_magnetometer.mx;
  magnetometer.my = ready_magnetometer.my;
  magnetometer.mz = ready_magnetometer.mz;
  magnetometer.temperature_cdeg = ready_magnetometer.temperature_cdeg;
  roll = ready_roll;
  pitch = ready_pitch;
  yaw = ready_yaw;
  six_axis_ready = 0U;
  attitude_ready = 0U;
  magnetometer_ready = 0U;
  __enable_irq();

  if (copy_six_axis != 0U)
  {
    IMU_OutputSixAxis(output_uart, &six_axis);
  }
  if (copy_magnetometer != 0U)
  {
    IMU_OutputMagnetometer(output_uart, &magnetometer);
  }
  if (copy_attitude != 0U)
  {
    IMU_OutputAttitude(output_uart, roll, pitch, yaw);
  }
}

/*
 * 获取 IMU 最新状态。
 * 调用前需要传入非空指针；函数内部关中断拷贝，保证线程安全。
 */
void IMU_Sensor_GetState(IMU_SensorState_t *state)
{
  if (state == NULL)
  {
    return;
  }

  __disable_irq();
  state->six_axis_valid = six_axis_seen;
  state->attitude_valid = attitude_seen;
  state->magnetometer_valid = magnetometer_seen;
  state->ax = ready_six_axis.ax;
  state->ay = ready_six_axis.ay;
  state->az = ready_six_axis.az;
  state->gx = ready_six_axis.gx;
  state->gy = ready_six_axis.gy;
  state->gz = ready_six_axis.gz;
  state->mx = ready_magnetometer.mx;
  state->my = ready_magnetometer.my;
  state->mz = ready_magnetometer.mz;
  state->mag_temperature_cdeg = ready_magnetometer.temperature_cdeg;
  state->roll = ready_roll;
  state->pitch = ready_pitch;
  state->yaw = ready_yaw;
  state->six_axis_update_ms = ready_six_axis_tick;
  state->attitude_update_ms = ready_attitude_tick;
  state->magnetometer_update_ms = ready_magnetometer_tick;
  __enable_irq();
}
