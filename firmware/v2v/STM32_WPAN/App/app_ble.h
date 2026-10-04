/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_ble.h
  * @author  MCD Application Team
  * @brief   Header for ble application
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef APP_BLE_H
#define APP_BLE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdint.h>

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/

typedef enum
{
  APP_BLE_IDLE,
  APP_BLE_LP_CONNECTING,
  APP_BLE_CONNECTED_SERVER,
  APP_BLE_CONNECTED_CLIENT,
  APP_BLE_ADV_FAST,
  APP_BLE_ADV_LP,
  APP_BLE_ADV_NON_CONN_FAST,
  APP_BLE_ADV_NON_CONN_LP,
  /* USER CODE BEGIN APP_BLE_ConnStatus_t */

  /* USER CODE END APP_BLE_ConnStatus_t */
} APP_BLE_ConnStatus_t;

/**
  * HCI Event Packet Types
  */
typedef __PACKED_STRUCT
{
  uint32_t *next;
  uint32_t *prev;
} BleEvtPacketHeader_t;

typedef __PACKED_STRUCT
{
  uint8_t   evtcode;
  uint8_t   plen;
  uint8_t   payload[1];
} BleEvt_t;

typedef __PACKED_STRUCT
{
  uint8_t   type;
  BleEvt_t  evt;
} BleEvtSerial_t;

typedef __PACKED_STRUCT __ALIGNED(4)
{
  BleEvtPacketHeader_t  header;
  BleEvtSerial_t        evtserial;
} BleEvtPacket_t;

typedef enum
{
  PROC_GAP_GEN_PHY_TOGGLE,
  PROC_GAP_GEN_CONN_TERMINATE,
  PROC_GATT_EXCHANGE_CONFIG,
  /* USER CODE BEGIN ProcGapGeneralId_t */

  /* USER CODE END ProcGapGeneralId_t */
}ProcGapGeneralId_t;

typedef enum
{
  PROC_GAP_PERIPH_ADVERTISE_START_LP,
  PROC_GAP_PERIPH_ADVERTISE_START_FAST,
  PROC_GAP_PERIPH_ADVERTISE_NON_CONN_START_LP,
  PROC_GAP_PERIPH_ADVERTISE_NON_CONN_START_FAST,
  PROC_GAP_PERIPH_ADVERTISE_STOP,
  PROC_GAP_PERIPH_ADVERTISE_DATA_UPDATE,
  PROC_GAP_PERIPH_CONN_PARAM_UPDATE,
  /* USER CODE BEGIN ProcGapPeripheralId_t */

  /* USER CODE END ProcGapPeripheralId_t */
}ProcGapPeripheralId_t;

typedef enum
{
  PROC_GAP_CENTRAL_SCAN_START,
  PROC_GAP_CENTRAL_SCAN_TERMINATE,
  /* USER CODE BEGIN ProcGapCentralId_t */

  /* USER CODE END ProcGapCentralId_t */
}ProcGapCentralId_t;

/* USER CODE BEGIN ET */

/*
 * 语音预警枚举。
 * 枚举值同时作为 MP3 语音模块的曲目编号，因此不要随意调整顺序。
 *   NONE:              无预警；
 *   LEFT_RISK:         左侧来车；
 *   RIGHT_RISK:        右侧来车；
 *   LEFT_FRONT_RISK:   左前方来车；
 *   RIGHT_FRONT_RISK:  右前方来车；
 *   GPS_WEAK:          GPS 信号弱，仅做粗略提醒。
 */
typedef enum
{
  APP_BLE_VOICE_NONE = 0,
  APP_BLE_VOICE_LEFT_RISK = 1,
  APP_BLE_VOICE_RIGHT_RISK = 2,
  APP_BLE_VOICE_LEFT_FRONT_RISK = 3,
  APP_BLE_VOICE_RIGHT_FRONT_RISK = 4,
  APP_BLE_VOICE_GPS_WEAK = 5
} APP_BLE_VoicePrompt_t;

/* 待播报的语音预警信息，由 app_ble 生产、ttl_uart2_router 消费 */
typedef struct
{
  APP_BLE_VoicePrompt_t prompt;  /* 语音方向/类型 */
  uint16_t device_id;            /* 对端设备 ID */
  uint32_t distance_cm;          /* 相对距离估算值，单位厘米 */
  int32_t y_front_cm;            /* 对端在本机前方为正、后方为负 */
  int32_t x_right_cm;            /* 对端在本机右侧为正、左侧为负 */
} APP_BLE_VoiceWarning_t;

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* External variables --------------------------------------------------------*/

/* USER CODE BEGIN EV */

/* USER CODE END EV */

/* Exported macro ------------------------------------------------------------*/
#define SCAN_WIN_MS(x) ((uint16_t)((x)/0.625f))
#define SCAN_INT_MS(x) ((uint16_t)((x)/0.625f))
#define CONN_INT_MS(x) ((uint16_t)((x)/1.25f))
#define CONN_SUP_TIMEOUT_MS(x) ((uint16_t)((x)/10.0f))
#define CONN_CE_LENGTH_MS(x) ((uint16_t)((x)/0.625f))

#define HCI_LE_ADVERTISING_REPORT_RSSI(p) \
        (*(int8_t*)((&((hci_le_advertising_report_event_rp0*)(p))-> \
                      Advertising_Report[0].Length_Data) + 1 + \
                    ((hci_le_advertising_report_event_rp0*)(p))-> \
                    Advertising_Report[0].Length_Data))
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void APP_BLE_Init(void);
APP_BLE_ConnStatus_t APP_BLE_Get_Server_Connection_Status(void);
void APP_BLE_Procedure_Gap_General(ProcGapGeneralId_t ProcGapGeneralId);
void APP_BLE_Procedure_Gap_Peripheral(ProcGapPeripheralId_t ProcGapPeripheralId);
const uint8_t* BleGetBdAddress(void);
/* USER CODE BEGIN EFP */

/* 从外部串口接收调试/测试数据（当前工程未使用，保留接口） */
void APP_BLE_UartRxData(const uint8_t *pData, uint16_t len);

/* 主循环周期调用，刷新本机状态负载并检查对端超时 */
void APP_BLE_Protocol_Tick(void);

/*
 * 取出一个待播报的语音预警。
 * 返回非 0 表示成功取出，调用者负责播放；
 * 返回 0 表示当前没有待播报预警。
 */
uint8_t APP_BLE_TakeVoiceWarning(APP_BLE_VoiceWarning_t *warning);

/* USER CODE END EFP */

#ifdef __cplusplus
}
#endif

#endif /*APP_BLE_H */
