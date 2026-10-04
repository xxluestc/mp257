/*
 * GP21 GNSS 模块驱动。
 *
 * 通过 UART 接收 NMEA-0183 RMC 语句，解析经纬度、速度、航向，
 * 并做静态漂移抑制和运动速度滞回，输出给上层 V2X 业务使用。
 */
#include "gp21_gnss.h"

#include <string.h>

/* NMEA 行缓冲区大小 */
#define GP21_NMEA_LINE_SIZE 128U
/* 经纬度内部缩放：1e-7 度 */
#define GP21_COORD_SCALE 10000000L

/*
 * 静态漂移抑制参数：
 * 当速度低于 GP21_STATIC_SPEED_MAX_CMS（1m/s）时，认为设备近似静止。
 * 若新坐标与上一次有效坐标偏差超过 GP21_STATIC_JUMP_LIMIT_1E7（约 20 米），
 * 则先当作“跳点”挂起，连续确认 GP21_STATIC_JUMP_CONFIRM_COUNT 次后才采信。
 */
#define GP21_STATIC_SPEED_MAX_CMS 100U
#define GP21_STATIC_JUMP_LIMIT_1E7 200L
#define GP21_STATIC_JUMP_CONFIRM_COUNT 2U

/*
 * 运动状态滞回参数：
 * 速度超过 2m/s 连续 3 次才确认“在运动”，避免低速噪声误判；
 * 速度低于 1m/s 连续 5 次才退出运动状态，避免走走停停时频繁切换。
 */
#define GP21_MOVING_ENTER_SPEED_CMS 200U
#define GP21_MOVING_EXIT_SPEED_CMS 100U
#define GP21_MOVING_ENTER_CONFIRM_COUNT 3U
#define GP21_MOVING_EXIT_CONFIRM_COUNT 5U

/* 当前正在接收的 NMEA 行 */
static char line_buffer[GP21_NMEA_LINE_SIZE];
/* 已接收完成、等待主循环解析的 RMC 行 */
static char ready_line[GP21_NMEA_LINE_SIZE];
static uint16_t line_length;
static volatile uint8_t line_ready;

/* 最新解析后的 GNSS 状态，中断与主循环共享 */
static GP21_GnssState_t latest_state;

/* 静态跳点挂起坐标与计数 */
static int32_t pending_jump_latitude;
static int32_t pending_jump_longitude;
static uint8_t pending_jump_count;

/* 运动速度确认状态机 */
static uint8_t moving_speed_valid;
static uint8_t moving_enter_count;
static uint8_t moving_exit_count;

/* 字符转十六进制数值，校验和解析用 */
static int8_t GP21_HexValue(char c)
{
  if ((c >= '0') && (c <= '9'))
  {
    return (int8_t)(c - '0');
  }
  if ((c >= 'A') && (c <= 'F'))
  {
    return (int8_t)(c - 'A' + 10);
  }
  if ((c >= 'a') && (c <= 'f'))
  {
    return (int8_t)(c - 'a' + 10);
  }

  return -1;
}

/*
 * 校验 NMEA 行尾 *XX 校验和。
 * 从 $ 之后到 * 之前所有字符异或，再与 * 后的两位十六进制比较。
 */
static uint8_t GP21_ChecksumOk(const char *line)
{
  const char *p;
  uint8_t checksum = 0U;
  int8_t hi;
  int8_t lo;

  if ((line == NULL) || (line[0] != '$'))
  {
    return 0U;
  }

  p = &line[1];
  while ((*p != '\0') && (*p != '*'))
  {
    checksum ^= (uint8_t)*p;
    p++;
  }

  if ((p[0] != '*') || (p[1] == '\0') || (p[2] == '\0'))
  {
    return 0U;
  }

  hi = GP21_HexValue(p[1]);
  lo = GP21_HexValue(p[2]);
  if ((hi < 0) || (lo < 0))
  {
    return 0U;
  }

  return (checksum == (uint8_t)(((uint8_t)hi << 4U) | (uint8_t)lo)) ? 1U : 0U;
}

/* 判断当前行是否为指定类型，例如 "RMC" */
static uint8_t GP21_IsSentenceType(const char *line, const char *type)
{
  if ((line == NULL) || (type == NULL) || (line[0] != '$'))
  {
    return 0U;
  }

  return ((line[3] == type[0]) &&
          (line[4] == type[1]) &&
          (line[5] == type[2]) &&
          (line[6] == ',')) ? 1U : 0U;
}

/*
 * 按逗号分割取 NMEA 字段。
 * index 从 0 开始（$ 后的第一个字段是 talker，字段 0 为 sentence type 后的第一个）。
 */
static uint8_t GP21_GetField(const char *line, uint8_t index, char *out, uint8_t out_size)
{
  const char *start;
  const char *end;
  uint8_t current = 0U;
  uint8_t len;

  if ((line == NULL) || (out == NULL) || (out_size == 0U))
  {
    return 0U;
  }

  start = (line[0] == '$') ? &line[1] : line;
  while (*start != '\0')
  {
    end = start;
    while ((*end != '\0') && (*end != ',') && (*end != '*'))
    {
      end++;
    }

    if (current == index)
    {
      len = (uint8_t)(end - start);
      if (len >= out_size)
      {
        len = (uint8_t)(out_size - 1U);
      }
      memcpy(out, start, len);
      out[len] = '\0';
      return 1U;
    }

    if ((*end == '\0') || (*end == '*'))
    {
      break;
    }

    start = end + 1U;
    current++;
  }

  out[0] = '\0';
  return 0U;
}

/*
 * 把 NMEA 度分格式（ddmm.mmmmmmm）转换为内部 1e-7 度整数。
 * hemisphere 为 'N'/'S'/'E'/'W'，用于确定正负。
 */
static uint8_t GP21_NmeaCoordToScaled(const char *value, char hemisphere, int32_t *scaled)
{
  int32_t integer = 0;
  int32_t degrees;
  int32_t minutes;
  int32_t minute_fraction = 0;
  uint8_t fraction_digits = 0U;
  const char *p;
  int32_t result;

  if ((value == NULL) || (scaled == NULL) || (value[0] == '\0'))
  {
    return 0U;
  }

  p = value;
  while ((*p >= '0') && (*p <= '9'))
  {
    integer = (integer * 10) + (int32_t)(*p - '0');
    p++;
  }

  degrees = integer / 100;
  minutes = integer % 100;
  if (minutes >= 60)
  {
    return 0U;
  }

  if (*p == '.')
  {
    p++;
    while ((*p >= '0') && (*p <= '9') && (fraction_digits < 7U))
    {
      minute_fraction = (minute_fraction * 10) + (int32_t)(*p - '0');
      fraction_digits++;
      p++;
    }
  }

  while (fraction_digits < 7U)
  {
    minute_fraction *= 10;
    fraction_digits++;
  }

  result = (degrees * GP21_COORD_SCALE) +
           (((minutes * GP21_COORD_SCALE) + minute_fraction) / 60);
  if ((hemisphere == 'S') || (hemisphere == 'W'))
  {
    result = -result;
  }
  else if ((hemisphere != 'N') && (hemisphere != 'E'))
  {
    return 0U;
  }

  *scaled = result;
  return 1U;
}

/*
 * 解析无符号十进制字符串并按 scale 缩放。
 * 例如 scale=1000 时，"12.34" 会被解析为 12340。
 */
static uint8_t GP21_ParseUnsignedDecimalScaled(const char *value, uint32_t scale, uint32_t *scaled)
{
  uint32_t integer = 0U;
  uint32_t fraction = 0U;
  uint32_t divisor = 1U;
  const char *p;

  if ((value == NULL) || (scaled == NULL) || (value[0] == '\0'))
  {
    return 0U;
  }

  p = value;
  while ((*p >= '0') && (*p <= '9'))
  {
    integer = (integer * 10U) + (uint32_t)(*p - '0');
    p++;
  }

  if (*p == '.')
  {
    p++;
    while ((*p >= '0') && (*p <= '9') && (divisor < scale))
    {
      fraction = (fraction * 10U) + (uint32_t)(*p - '0');
      divisor *= 10U;
      p++;
    }
  }

  while (divisor < scale)
  {
    fraction *= 10U;
    divisor *= 10U;
  }

  *scaled = (integer * scale) + fraction;
  return 1U;
}

/* 把 RMC 速度字段（节）转换为厘米/秒 */
static uint16_t GP21_SpeedKnotsToCms(const char *speed_knots)
{
  uint32_t knots_milli;
  uint64_t cms;

  if (GP21_ParseUnsignedDecimalScaled(speed_knots, 1000U, &knots_milli) == 0U)
  {
    return 0U;
  }

  cms = (((uint64_t)knots_milli * 5144ULL) + 50000ULL) / 100000ULL;
  return (cms > 65535ULL) ? 65535U : (uint16_t)cms;
}

/* 把 RMC 航向字段（度）转换为 0.01 度 */
static uint16_t GP21_CourseToCdeg(const char *course_deg)
{
  uint32_t cdeg;

  if (GP21_ParseUnsignedDecimalScaled(course_deg, 100U, &cdeg) == 0U)
  {
    return 0U;
  }

  while (cdeg >= 36000U)
  {
    cdeg -= 36000U;
  }

  return (uint16_t)cdeg;
}

static uint32_t GP21_AbsDiff32(int32_t a, int32_t b)
{
  return (a >= b) ? (uint32_t)(a - b) : (uint32_t)(b - a);
}

/*
 * 速度运动状态滞回滤波。
 * 返回 0 表示当前判定为静止或低速不可信；否则返回实际速度值。
 * 用 enter/exit 两个阈值和连续计数，避免低速抖动。
 */
static uint16_t GP21_FilterSpeedCms(uint16_t speed_cms)
{
  if (moving_speed_valid == 0U)
  {
    if (speed_cms >= GP21_MOVING_ENTER_SPEED_CMS)
    {
      if (moving_enter_count < GP21_MOVING_ENTER_CONFIRM_COUNT)
      {
        moving_enter_count++;
      }
      if (moving_enter_count >= GP21_MOVING_ENTER_CONFIRM_COUNT)
      {
        moving_speed_valid = 1U;
        moving_exit_count = 0U;
        return speed_cms;
      }
    }
    else
    {
      moving_enter_count = 0U;
    }

    return 0U;
  }

  if (speed_cms <= GP21_MOVING_EXIT_SPEED_CMS)
  {
    if (moving_exit_count < GP21_MOVING_EXIT_CONFIRM_COUNT)
    {
      moving_exit_count++;
    }
    if (moving_exit_count >= GP21_MOVING_EXIT_CONFIRM_COUNT)
    {
      moving_speed_valid = 0U;
      moving_enter_count = 0U;
      return 0U;
    }
  }
  else
  {
    moving_exit_count = 0U;
  }

  return speed_cms;
}

/*
 * 静态漂移/跳点检测。
 * 低速时若坐标突变，先挂起，连续在同一位置出现多次才认为真的移动了。
 * 返回 1 表示当前点是跳点，应丢弃坐标但保留速度和航向。
 */
static uint8_t GP21_IsStaticJump(int32_t latitude,
                                 int32_t longitude,
                                 uint16_t speed_cms)
{
  GP21_GnssState_t state;
  uint32_t dlat;
  uint32_t dlon;

  if (speed_cms >= GP21_STATIC_SPEED_MAX_CMS)
  {
    pending_jump_count = 0U;
    return 0U;
  }

  __disable_irq();
  state = latest_state;
  __enable_irq();

  if (state.valid == 0U)
  {
    pending_jump_count = 0U;
    return 0U;
  }

  dlat = GP21_AbsDiff32(latitude, state.latitude_1e7);
  dlon = GP21_AbsDiff32(longitude, state.longitude_1e7);
  if ((dlat <= (uint32_t)GP21_STATIC_JUMP_LIMIT_1E7) &&
      (dlon <= (uint32_t)GP21_STATIC_JUMP_LIMIT_1E7))
  {
    pending_jump_count = 0U;
    return 0U;
  }

  if ((pending_jump_count > 0U) &&
      (GP21_AbsDiff32(latitude, pending_jump_latitude) <= (uint32_t)GP21_STATIC_JUMP_LIMIT_1E7) &&
      (GP21_AbsDiff32(longitude, pending_jump_longitude) <= (uint32_t)GP21_STATIC_JUMP_LIMIT_1E7))
  {
    pending_jump_count++;
  }
  else
  {
    pending_jump_latitude = latitude;
    pending_jump_longitude = longitude;
    pending_jump_count = 1U;
  }

  if (pending_jump_count >= GP21_STATIC_JUMP_CONFIRM_COUNT)
  {
    pending_jump_count = 0U;
    return 0U;
  }

  return 1U;
}

/*
 * 保存解析后的 GNSS 状态。
 * 无效时清空内部状态，并复位漂移抑制和运动状态机。
 */
static void GP21_StoreState(uint8_t valid,
                            int32_t latitude,
                            int32_t longitude,
                            uint16_t speed_cms,
                            uint16_t heading_cdeg)
{
  __disable_irq();
  latest_state.valid = valid;
  latest_state.latitude_1e7 = valid ? latitude : 0;
  latest_state.longitude_1e7 = valid ? longitude : 0;
  latest_state.speed_cms = valid ? speed_cms : 0U;
  latest_state.heading_cdeg = valid ? heading_cdeg : 0U;
  latest_state.pos_quality = valid ? 3U : 0U;
  latest_state.last_update_ms = HAL_GetTick();
  __enable_irq();

  if (valid == 0U)
  {
    pending_jump_count = 0U;
    moving_speed_valid = 0U;
    moving_enter_count = 0U;
    moving_exit_count = 0U;
  }
}

/*
 * 当坐标被判定为跳点时，只更新速度和航向、刷新时间戳，
 * 保持旧坐标不变，避免地图/方向因单点跳变而抖动。
 */
static void GP21_RefreshHeldState(uint16_t speed_cms, uint16_t heading_cdeg)
{
  __disable_irq();
  if (latest_state.valid != 0U)
  {
    latest_state.speed_cms = speed_cms;
    latest_state.heading_cdeg = heading_cdeg;
    latest_state.pos_quality = 3U;
    latest_state.last_update_ms = HAL_GetTick();
  }
  __enable_irq();
}

/*
 * 以下 Append* 工具函数用于无 printf 环境，把数字/字符串写入定长缓冲区。
 */

static uint16_t GP21_AppendChar(char *dst, uint16_t pos, uint16_t size, char c)
{
  if ((pos + 1U) < size)
  {
    dst[pos++] = c;
    dst[pos] = '\0';
  }

  return pos;
}

static uint16_t GP21_AppendString(char *dst, uint16_t pos, uint16_t size, const char *src)
{
  while ((src != NULL) && (*src != '\0'))
  {
    pos = GP21_AppendChar(dst, pos, size, *src);
    src++;
  }

  return pos;
}

static uint16_t GP21_AppendUInt(char *dst, uint16_t pos, uint16_t size, uint32_t value)
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
    pos = GP21_AppendChar(dst, pos, size, tmp[--len]);
  }

  return pos;
}

/* 把 1e-7 度整数格式化为“度.七位小数” */
static uint16_t GP21_AppendScaledCoord(char *dst, uint16_t pos, uint16_t size, int32_t value)
{
  uint32_t abs_value;
  uint32_t integer;
  uint32_t fraction;
  uint32_t divisor = 1000000U;

  if (value < 0)
  {
    pos = GP21_AppendChar(dst, pos, size, '-');
    abs_value = (uint32_t)(-value);
  }
  else
  {
    abs_value = (uint32_t)value;
  }

  integer = abs_value / (uint32_t)GP21_COORD_SCALE;
  fraction = abs_value % (uint32_t)GP21_COORD_SCALE;

  pos = GP21_AppendUInt(dst, pos, size, integer);
  pos = GP21_AppendChar(dst, pos, size, '.');
  while (divisor > 0U)
  {
    pos = GP21_AppendChar(dst, pos, size, (char)('0' + (fraction / divisor)));
    fraction %= divisor;
    divisor /= 10U;
  }

  return pos;
}

/* 把 0.01 度格式化为“度.两位小数” */
static uint16_t GP21_AppendCdeg(char *dst, uint16_t pos, uint16_t size, uint16_t value)
{
  pos = GP21_AppendUInt(dst, pos, size, (uint32_t)(value / 100U));
  pos = GP21_AppendChar(dst, pos, size, '.');
  pos = GP21_AppendChar(dst, pos, size, (char)('0' + ((value / 10U) % 10U)));
  pos = GP21_AppendChar(dst, pos, size, (char)('0' + (value % 10U)));

  return pos;
}

/* 输出有效定位日志 */
static void GP21_OutputFix(UART_HandleTypeDef *output_uart,
                           int32_t latitude,
                           char ns,
                           int32_t longitude,
                           char ew,
                           uint16_t speed_cms,
                           uint16_t heading_cdeg)
{
  char msg[128] = {0};
  uint16_t len = 0U;

  if (output_uart == NULL)
  {
    return;
  }

  len = GP21_AppendString(msg, len, sizeof(msg), "[GPS] fix lat=");
  len = GP21_AppendScaledCoord(msg, len, sizeof(msg), latitude);
  len = GP21_AppendChar(msg, len, sizeof(msg), ',');
  len = GP21_AppendChar(msg, len, sizeof(msg), ns);
  len = GP21_AppendString(msg, len, sizeof(msg), " lon=");
  len = GP21_AppendScaledCoord(msg, len, sizeof(msg), longitude);
  len = GP21_AppendChar(msg, len, sizeof(msg), ',');
  len = GP21_AppendChar(msg, len, sizeof(msg), ew);
  len = GP21_AppendString(msg, len, sizeof(msg), " speed=");
  len = GP21_AppendUInt(msg, len, sizeof(msg), speed_cms);
  len = GP21_AppendString(msg, len, sizeof(msg), "cm/s heading=");
  len = GP21_AppendCdeg(msg, len, sizeof(msg), heading_cdeg);
  len = GP21_AppendString(msg, len, sizeof(msg), "deg");
  len = GP21_AppendString(msg, len, sizeof(msg), "\r\n");

  (void)HAL_UART_Transmit(output_uart, (uint8_t *)msg, len, 50U);
}

/* 输出无效定位日志，所有字段置 0 */
static void GP21_OutputInvalidFix(UART_HandleTypeDef *output_uart)
{
  GP21_OutputFix(output_uart, 0, 'N', 0, 'E', 0U, 0U);
}

/* 输出校验失败或类型不符日志 */
static void GP21_OutputFail(UART_HandleTypeDef *output_uart)
{
  char msg[16] = {0};
  uint16_t len = 0U;

  if (output_uart == NULL)
  {
    return;
  }

  len = GP21_AppendString(msg, len, sizeof(msg), "[GPS] fail\r\n");

  (void)HAL_UART_Transmit(output_uart, (uint8_t *)msg, len, 50U);
}

/*
 * 解析一条 RMC 语句：
 *  1) 校验和与语句类型检查；
 *  2) 状态字段判断定位是否有效；
 *  3) 解析经纬度、速度、航向；
 *  4) 经速度和静态跳点滤波后保存状态；
 *  5) 输出日志。
 */
static void GP21_ParseLine(UART_HandleTypeDef *output_uart, const char *line)
{
  char status[2];
  char lat[16];
  char ns[2];
  char lon[16];
  char ew[2];
  char speed[12];
  char course[12];
  int32_t latitude;
  int32_t longitude;
  uint16_t speed_cms = 0U;
  uint16_t heading_cdeg = 0U;

  if ((GP21_ChecksumOk(line) == 0U) ||
      (GP21_IsSentenceType(line, "RMC") == 0U))
  {
    GP21_OutputFail(output_uart);
    return;
  }

  if ((GP21_GetField(line, 2U, status, sizeof(status)) == 0U) ||
      (status[0] != 'A'))
  {
    GP21_StoreState(0U, 0, 0, 0U, 0U);
    GP21_OutputInvalidFix(output_uart);
    return;
  }

  if ((GP21_GetField(line, 3U, lat, sizeof(lat)) == 0U) ||
      (GP21_GetField(line, 4U, ns, sizeof(ns)) == 0U) ||
      (GP21_GetField(line, 5U, lon, sizeof(lon)) == 0U) ||
      (GP21_GetField(line, 6U, ew, sizeof(ew)) == 0U))
  {
    GP21_StoreState(0U, 0, 0, 0U, 0U);
    GP21_OutputInvalidFix(output_uart);
    return;
  }

  if ((GP21_NmeaCoordToScaled(lat, ns[0], &latitude) != 0U) &&
      (GP21_NmeaCoordToScaled(lon, ew[0], &longitude) != 0U))
  {
    if (GP21_GetField(line, 7U, speed, sizeof(speed)) != 0U)
    {
      speed_cms = GP21_SpeedKnotsToCms(speed);
    }
    speed_cms = GP21_FilterSpeedCms(speed_cms);
    if (GP21_GetField(line, 8U, course, sizeof(course)) != 0U)
    {
      heading_cdeg = GP21_CourseToCdeg(course);
    }

    if (GP21_IsStaticJump(latitude, longitude, speed_cms) == 0U)
    {
      GP21_StoreState(1U, latitude, longitude, speed_cms, heading_cdeg);
    }
    else
    {
      GP21_RefreshHeldState(speed_cms, heading_cdeg);
    }
    GP21_OutputFix(output_uart, latitude, ns[0], longitude, ew[0], speed_cms, heading_cdeg);
  }
  else
  {
    GP21_StoreState(0U, 0, 0, 0U, 0U);
    GP21_OutputInvalidFix(output_uart);
  }
}

/* 初始化接收缓冲和状态机 */
void GP21_Gnss_Init(void)
{
  line_length = 0U;
  line_ready = 0U;
  memset(&latest_state, 0, sizeof(latest_state));
  pending_jump_count = 0U;
  moving_speed_valid = 0U;
  moving_enter_count = 0U;
  moving_exit_count = 0U;
}

/*
 * 逐字节接收 NMEA。
 * 收到 '$' 开始新行，收到 \r/\n 结束；
 * 仅当行类型为 RMC 时才把完整行推入 ready_line 供主循环解析。
 */
GP21_GnssRxResult_t GP21_Gnss_ProcessByte(uint8_t byte)
{
  GP21_GnssRxResult_t result = GP21_GNSS_RX_PENDING;

  if (byte == '$')
  {
    line_length = 0U;
    line_buffer[line_length++] = (char)byte;
    return result;
  }

  if (line_length == 0U)
  {
    return result;
  }

  if ((byte == '\r') || (byte == '\n'))
  {
    line_buffer[line_length] = '\0';
    if ((line_length > 0U) && (GP21_IsSentenceType(line_buffer, "RMC") != 0U))
    {
      result = GP21_GNSS_RX_RMC_LINE;
      if (line_ready == 0U)
      {
        memcpy(ready_line, line_buffer, line_length + 1U);
        line_ready = 1U;
      }
    }
    line_length = 0U;
    return result;
  }

  if (line_length < (GP21_NMEA_LINE_SIZE - 1U))
  {
    line_buffer[line_length++] = (char)byte;
  }
  else
  {
    line_length = 0U;
  }

  return result;
}

/* 主循环调用：取走已就绪的 RMC 行并解析 */
void GP21_Gnss_ProcessReadyData(UART_HandleTypeDef *output_uart)
{
  char line[GP21_NMEA_LINE_SIZE];

  if (line_ready == 0U)
  {
    return;
  }

  __disable_irq();
  memcpy(line, ready_line, sizeof(line));
  line_ready = 0U;
  __enable_irq();

  GP21_ParseLine(output_uart, line);
}

/* 线程安全地获取最新 GNSS 状态 */
void GP21_Gnss_GetState(GP21_GnssState_t *state)
{
  if (state == NULL)
  {
    return;
  }

  __disable_irq();
  *state = latest_state;
  __enable_irq();
}
