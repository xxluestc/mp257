/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_ble.c
  * @author  MCD Application Team
  * @brief   BLE Application
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

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "app_common.h"
#include "log_module.h"
#include "ble.h"
#include "app_ble.h"
#include "host_stack_if.h"
#include "ll_sys_if.h"
#include "stm32_rtos.h"
#include "otp.h"
#include "stm32_timer.h"
#include "stm_list.h"
#include "advanced_memory_manager.h"
#include "blestack.h"
#include "nvm.h"
#include "a.h"
#include "a_app.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "gp21_gnss.h"
#include "imu_sensor.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Security parameters structure */
typedef struct
{
  /* IO capability of the device */
  uint8_t ioCapability;

  /**
   * Authentication requirement of the device
   * Man In the Middle protection required?
   */
  uint8_t mitm_mode;

  /* Bonding mode of the device */
  uint8_t bonding_mode;

  /**
   * this variable indicates whether to use a fixed pin
   * during the pairing process or a passkey has to be
   * requested to the application during the pairing process
   * 0 implies use fixed pin and 1 implies request for passkey
   */
  uint8_t Use_Fixed_Pin;

  /* Minimum encryption key size requirement */
  uint8_t encryptionKeySizeMin;

  /* Maximum encryption key size requirement */
  uint8_t encryptionKeySizeMax;

  /**
   * fixed pin to be used in the pairing process if
   * Use_Fixed_Pin is set to 1
   */
  uint32_t Fixed_Pin;

  /**
   * this flag indicates whether the host has to initiate
   * the security, wait for pairing or does not have any security
   * requirements.
   * 0x00 : no security required
   * 0x01 : host should initiate security by sending the slave security
   *        request command
   * 0x02 : host need not send the clave security request but it
   * has to wait for paiirng to complete before doing any other
   * processing
   */
  uint8_t initiateSecurity;
  /* USER CODE BEGIN tSecurityParams */

  /* USER CODE END tSecurityParams */
}SecurityParams_t;

/* Global context contains all BLE common variables. */
typedef struct
{
  /* Security requirements of the host */
  SecurityParams_t bleSecurityParam;

  /* GAP service handle */
  uint16_t gapServiceHandle;

  /* Device name characteristic handle */
  uint16_t devNameCharHandle;

  /* Appearance characteristic handle */
  uint16_t appearanceCharHandle;

  /**
   * connection handle of the current active connection
   * When not in connection, the handle is set to 0xFFFF
   */
  uint16_t connectionHandle;

  /* USER CODE BEGIN BleGlobalContext_t */

  /* USER CODE END BleGlobalContext_t */
}BleGlobalContext_t;

typedef struct
{
  BleGlobalContext_t BleApplicationContext_legacy;
  APP_BLE_ConnStatus_t Device_Connection_Status;
  /* USER CODE BEGIN PTD_1 */

  /* USER CODE END PTD_1 */
}BleApplicationContext_t;

/* Private defines -----------------------------------------------------------*/
/* GATT buffer size (in bytes)*/
#define BLE_GATT_BUF_SIZE \
          BLE_TOTAL_BUFFER_SIZE_GATT(CFG_BLE_NUM_GATT_ATTRIBUTES, \
                                     CFG_BLE_NUM_GATT_SERVICES, \
                                     CFG_BLE_ATT_VALUE_ARRAY_SIZE)

#define MBLOCK_COUNT              (BLE_MBLOCKS_CALC(PREP_WRITE_LIST_SIZE, \
                                                    CFG_BLE_ATT_MTU_MAX, \
                                                    CFG_BLE_NUM_LINK) \
                                   + CFG_BLE_MBLOCK_COUNT_MARGIN)

#define BLE_DYN_ALLOC_SIZE \
        (BLE_TOTAL_BUFFER_SIZE(CFG_BLE_NUM_LINK, MBLOCK_COUNT))

/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
static tListNode BleAsynchEventQueue;

static uint8_t a_BdAddr[BD_ADDR_SIZE];
/* Identity root key used to derive IRK and DHK(Legacy) */
static uint8_t a_BLE_CfgIrValue[16];

/* Encryption root key used to derive LTK(Legacy) and CSRK */
static uint8_t a_BLE_CfgErValue[16];
static BleApplicationContext_t bleAppContext;
A_APP_ConnHandleNotEvt_t AHandleNotification;

static char a_GapDeviceName[] = {  'M', 'R', 'N', 'O', 'D', 'E', '0', '1' }; /* Gap Device Name */

/* Advertising Data */
uint8_t a_AdvData[31];
uint64_t buffer_nvm[CFG_BLEPLAT_NVM_MAX_SIZE] = {0};

static AMM_VirtualMemoryCallbackFunction_t APP_BLE_ResumeFlowProcessCb;

/* Host stack init variables */
static uint32_t buffer[DIVC(BLE_DYN_ALLOC_SIZE, 4)];
static uint32_t gatt_buffer[DIVC(BLE_GATT_BUF_SIZE, 4)];
static BleStack_init_t pInitParams;

/* USER CODE BEGIN PV */
enum
{
  APP_BLE_BRIDGE_STATE_PAYLOAD_LEN = 16,
  APP_BLE_BRIDGE_MFG_PAYLOAD_LEN = 4 + APP_BLE_BRIDGE_STATE_PAYLOAD_LEN,
  APP_BLE_BRIDGE_UART_RX_BUF_LEN = 64,
  APP_BLE_BRIDGE_UART_MAX_PAYLOAD_LEN = 32,
  APP_BLE_BRIDGE_ADV_REFRESH_MS = 1000,
  APP_BLE_BRIDGE_UART_STATE_TIMEOUT_MS = 500,
  //多久没收到对端数据，就认为对端消失。单位：毫秒
  APP_BLE_BRIDGE_PEER_STALE_MS = 2000,
  //多久处理一次对端数据，做距离趋势判断。
  APP_BLE_BRIDGE_WARN_INTERVAL_MS = 1200,
  //方向必须稳定这么长时间，才会真正入队语音。单位：毫秒
  APP_BLE_BRIDGE_VOICE_RISK_HOLD_MS = 1500,
  //风险条件连续解除这么长时间后，才允许下一次语音事件。单位：毫秒
  APP_BLE_BRIDGE_VOICE_REARM_HOLD_MS = 3000
};

#define APP_BLE_BRIDGE_COMPANY_ID_L             0x34U
#define APP_BLE_BRIDGE_COMPANY_ID_H             0x12U
#define APP_BLE_BRIDGE_MAGIC_L                  0xAAU
#define APP_BLE_BRIDGE_MAGIC_H                  0x55U
#define APP_BLE_BRIDGE_PROTO_VER                0x02U
#define APP_BLE_BRIDGE_LOCAL_DEVICE_ID          0x1001U
#define APP_BLE_BRIDGE_EXPECTED_PEER_ID         0x1002U
/* Protocol v1 contained a fixed bring-up payload. Production traffic is v2 only. */
#define APP_BLE_BRIDGE_UART_FRAME_H0            0xA5U
#define APP_BLE_BRIDGE_UART_FRAME_H1            0x5AU
#define APP_BLE_BRIDGE_UART_TYPE_TX_STATE       0x01U
#define APP_BLE_BRIDGE_UART_TYPE_LOCAL_STATE    0x81U
#define APP_BLE_BRIDGE_UART_TYPE_SCAN_STATE     0x82U
#define APP_BLE_BRIDGE_RADIO_ENABLE             1U
#define APP_BLE_BRIDGE_BINARY_DEBUG_FRAME_ENABLE 0U
/* 1: 打印每次收到的对端原始负载；0: 关闭，减少刷屏 */
#define APP_BLE_BRIDGE_PEER_DEBUG_ENABLE        0U
/* 1: 调试时强制认为对端在动，忽略对端上报的运动状态；0: 正常逻辑 */
#define APP_BLE_BRIDGE_FORCE_PEER_MOVING        0U

#define APP_BLE_BRIDGE_OFF_PROTO                0U
#define APP_BLE_BRIDGE_OFF_DEVICE_ID            1U
#define APP_BLE_BRIDGE_OFF_MSG_COUNTER          3U
#define APP_BLE_BRIDGE_OFF_STATUS_FLAGS         4U
#define APP_BLE_BRIDGE_OFF_SPEED_Q              5U
#define APP_BLE_BRIDGE_OFF_HEADING_Q            6U
#define APP_BLE_BRIDGE_OFF_YAW_RATE_Q           8U
#define APP_BLE_BRIDGE_OFF_LAT_LOW16            9U
#define APP_BLE_BRIDGE_OFF_LON_LOW16            11U
#define APP_BLE_BRIDGE_OFF_POS_QUALITY          13U
#define APP_BLE_BRIDGE_OFF_GPS_AGE_Q            14U
#define APP_BLE_BRIDGE_OFF_RESERVED             15U

#define APP_BLE_BRIDGE_STATUS_MOVING            0x01U
#define APP_BLE_BRIDGE_STATUS_TURNING           0x02U
#define APP_BLE_BRIDGE_STATUS_BRAKING           0x04U
#define APP_BLE_BRIDGE_STATUS_GPS_VALID         0x08U
#define APP_BLE_BRIDGE_STATUS_HIGH_RISK         0x10U
#define APP_BLE_BRIDGE_STATUS_LOW_BATTERY       0x20U
#define APP_BLE_BRIDGE_STATUS_IMU_VALID         0x40U
#define APP_BLE_BRIDGE_STATUS_LOW_CONFIDENCE    0x80U

#define APP_BLE_BRIDGE_GPS_VALID_MAX_AGE_MS     1500U
#define APP_BLE_BRIDGE_IMU_VALID_MAX_AGE_MS     1000U
#define APP_BLE_BRIDGE_GPS_AGE_UNIT_MS          20U
//关注距离，8 米内即使不靠近也强制关注
#define APP_BLE_BRIDGE_TARGET_CLOSE_CM          800
//关注距离，30 米内即使靠近也强制关注
#define APP_BLE_BRIDGE_TARGET_NEAR_CM           3000
//距离趋势阈值，单位：厘米，要求相邻两次距离变化超过这个值
#define APP_BLE_BRIDGE_DISTANCE_TREND_THRESHOLD_CM 300U
/* Minimum interval between any two voice reports. */
//两次语音播报之间至少间隔这么长时间。单位：毫秒
#define APP_BLE_BRIDGE_VOICE_REPORT_INTERVAL_MS 3000U
#define APP_BLE_BRIDGE_PEER_GPS_AGE_CHECK_ENABLE 1U
#define APP_BLE_BRIDGE_PEER_GPS_MAX_AGE_Q       75U
#define APP_BLE_BRIDGE_GPS_COURSE_MIN_SPEED_CMS 200U
#define APP_BLE_BRIDGE_HEADING_CAL_MIN_SPEED_CMS 200U
#define APP_BLE_BRIDGE_HEADING_CAL_CONFIRM_COUNT 3U
#define APP_BLE_BRIDGE_HEADING_CAL_MAX_STEP_CDEG 1500
#define APP_BLE_BRIDGE_MOVING_MIN_SPEED_Q       2U
#define APP_BLE_BRIDGE_TURNING_MIN_YAW_RATE_Q   4
#define APP_BLE_BRIDGE_IMU_YAW_TO_HEADING_SIGN  (1)
#define APP_BLE_BRIDGE_IMU_HEADING_OFFSET_CDEG  18000
#define APP_BLE_BRIDGE_HEADING_SRC_NONE         0U
#define APP_BLE_BRIDGE_HEADING_SRC_GPS          1U
#define APP_BLE_BRIDGE_HEADING_SRC_IMU          2U
#define APP_BLE_BRIDGE_RANGE_TREND_UNKNOWN      0U
#define APP_BLE_BRIDGE_RANGE_TREND_APPROACHING  1U
#define APP_BLE_BRIDGE_RANGE_TREND_RECEDING     2U

static uint8_t a_ScanRspData[31];
static uint8_t a_AdvDataLen;
static uint8_t a_ScanRspDataLen;
static uint8_t a_StatePayload[APP_BLE_BRIDGE_STATE_PAYLOAD_LEN];
static uint8_t a_UartRxBuf[APP_BLE_BRIDGE_UART_RX_BUF_LEN];
static uint16_t uartRxLen;
static uint8_t msgCounter;
static uint32_t lastAdvRefreshTick;
static uint8_t bridgeStarted;
static uint32_t filterMatchCount;
static uint8_t bridgeUartStateActive;
static uint8_t bridgeUartStateSeen;
static uint32_t bridgeUartStateLastRxTick;
static uint8_t bridgeHeadingCalValid;
static int32_t bridgeImuHeadingOffsetCdeg = APP_BLE_BRIDGE_IMU_HEADING_OFFSET_CDEG;
static uint32_t bridgeHeadingCalGpsTick;
static int32_t bridgeHeadingCalCandidateCdeg;
static uint8_t bridgeHeadingCalCandidateCount;

typedef struct
{
  uint8_t valid;
  uint16_t device_id;
  uint8_t msg_counter;
  uint8_t status_flags;
  uint8_t speed_q;
  uint16_t heading_q;
  int8_t yaw_rate_q;
  uint16_t lat_low16;
  uint16_t lon_low16;
  uint8_t pos_quality;
  uint8_t gps_age_q;
  uint8_t rssi_valid;
  int8_t rssi;
  uint32_t last_rx_ms;
  int32_t filtered_y_front_cm;
  int32_t filtered_x_right_cm;
  uint32_t last_distance_cm;
  uint8_t filter_valid;
  uint8_t closing_count;
  uint8_t range_trend;
  uint8_t heading_source;
  uint32_t last_warn_ms;
  APP_BLE_VoicePrompt_t voice_candidate_prompt;
  uint32_t voice_candidate_start_ms;
  APP_BLE_VoicePrompt_t voice_last_prompt;
} Bridge_PeerTrack_t;

static Bridge_PeerTrack_t peerTrack;
static APP_BLE_VoiceWarning_t pendingVoiceWarning;
/* 同一连续风险已经完成过一次播报；只有稳定解除后才清零。 */
static uint8_t bridgeVoiceReportValid;
static uint32_t bridgeVoiceLastReportMs;
static uint8_t bridgeVoiceClearPending;
static uint32_t bridgeVoiceClearStartMs;

/* 普通调试日志最小打印间隔，防止串口被刷爆。单位：毫秒 */
static uint32_t bridgeDebugLastPrintMs;
#define APP_BLE_BRIDGE_DEBUG_PRINT_INTERVAL_MS 1000U

/* USER CODE END PV */

/* Global variables ----------------------------------------------------------*/

/* USER CODE BEGIN GV */

/* USER CODE END GV */

/* Private function prototypes -----------------------------------------------*/
static void BleStack_Process_BG(void);
static void Ble_UserEvtRx(void);
static void BLE_ResumeFlowProcessCallback(void);
static void Ble_Hci_Gap_Gatt_Init(void);
static const uint8_t* BleGenerateBdAddress(void);
static const uint8_t* BleGenerateIRValue(void);
static const uint8_t* BleGenerateERValue(void);
static void gap_cmd_resp_wait(void);
static void gap_cmd_resp_release(void);
static uint8_t HOST_BLE_Init(void);
/* USER CODE BEGIN PFP */
static void Bridge_StatePayloadInit(void);
static void Bridge_StatePayloadTouch(void);
static uint16_t Bridge_GetU16LE(const uint8_t *p);
static uint16_t Bridge_Coord1e7ToLow16(int32_t coord_1e7);
static uint8_t Bridge_EncodeSpeedQ(uint16_t speed_cms);
static uint16_t Bridge_EncodeHeadingQ(uint16_t heading_cdeg);
static int32_t Bridge_NormalizeHeadingCdeg(int32_t heading_cdeg);
static int32_t Bridge_HeadingDeltaCdeg(int32_t target_cdeg, int32_t reference_cdeg);
static int32_t Bridge_ImuYawToHeadingCdeg(int16_t yaw_raw);
static uint16_t Bridge_EncodeImuYawHeadingQ(int16_t yaw_raw);
static int8_t Bridge_EncodeYawRateQ(int16_t gyro_z_raw);
static uint8_t Bridge_EncodeGpsAgeQ(uint32_t now, uint32_t gps_tick, uint8_t gps_valid);
static void Bridge_UpdateHeadingCalibration(const GP21_GnssState_t *gps,
                                            const IMU_SensorState_t *imu,
                                            uint32_t now);
static void Bridge_BuildStatePayloadFromSensors(void);
static void Bridge_FillAdvertisingData(void);
static void Bridge_UpdateAdvertisingData(void);
static void Bridge_StartScan(void);
static uint8_t Bridge_UartChecksum(uint8_t type, uint8_t len, const uint8_t *payload);
static void Bridge_UartSendFrame(uint8_t type, const uint8_t *payload, uint8_t len);
static void Bridge_UartProcessRx(void);
static void Bridge_HandleRawAdvertisingReports(const uint8_t *pData);
static uint8_t Bridge_FindAdType(const uint8_t *adData, uint8_t adLen, uint8_t adType, const uint8_t **ppData, uint8_t *pDataLen);
static uint8_t Bridge_FilterAdvertisingReport(const Advertising_Report_t *report, uint8_t *payload);
static void Bridge_HandlePeerPayload(const uint8_t *payload, int8_t rssi, uint8_t rssi_valid);
static void Bridge_CheckPeerTimeout(void);
static void Bridge_DebugPayload(const char *prefix, const uint8_t *payload, int8_t rssi, uint8_t rssi_valid);
static uint8_t Bridge_IsImuHeadingFresh(const IMU_SensorState_t *imu, uint32_t now);
static int32_t Bridge_LongitudeScaleQ15(int32_t latitude_1e7);
static uint8_t Bridge_IsPeerMoving(uint8_t status_flags);
static uint8_t Bridge_QueueVoiceWarning(APP_BLE_VoicePrompt_t prompt,
                                        uint16_t device_id,
                                        uint32_t distance_cm,
                                        int32_t y_front_cm,
                                        int32_t x_right_cm);
static void Bridge_ResetVoiceCandidate(void);
static void Bridge_CancelPendingVoiceWarning(void);
static void Bridge_ResetVoiceEvent(void);
static void Bridge_UpdateVoiceRearm(uint32_t now);
static void Bridge_UpdateVoiceCandidate(APP_BLE_VoicePrompt_t prompt,
                                        uint16_t device_id,
                                        uint32_t distance_cm,
                                        int32_t y_front_cm,
                                        int32_t x_right_cm,
                                        uint32_t now);
static APP_BLE_VoicePrompt_t Bridge_SelectDirectionalVoicePrompt(int32_t y_front_cm,
                                                                 int32_t x_right_cm);
static const char *Bridge_MotionText(uint8_t status_flags);
static const char *Bridge_RssiDistanceText(int8_t rssi, uint8_t rssi_valid);
static const char *Bridge_DirectionText(int32_t y_front_cm, int32_t x_right_cm);
static const char *Bridge_VoicePromptText(APP_BLE_VoicePrompt_t prompt);
static const char *Bridge_RangeTrendText(uint8_t trend);
static void Bridge_InfoPrint(const char *text);
static void Bridge_DebugPrint(const char *text);
static uint16_t Bridge_AppendString(char *dst, uint16_t pos, uint16_t size, const char *src);
static uint16_t Bridge_AppendUInt(char *dst, uint16_t pos, uint16_t size, uint32_t value);
static uint16_t Bridge_AppendInt(char *dst, uint16_t pos, uint16_t size, int32_t value);

/* USER CODE END PFP */

/* External variables --------------------------------------------------------*/

/* USER CODE BEGIN EV */

/* USER CODE END EV */

/* Functions Definition ------------------------------------------------------*/
void APP_BLE_Init(void)
{
  /* USER CODE BEGIN APP_BLE_Init_1 */
  Bridge_StatePayloadInit();
  Bridge_FillAdvertisingData();

  /* USER CODE END APP_BLE_Init_1 */

  LST_init_head(&BleAsynchEventQueue);

  /* Register BLE Host tasks */
  UTIL_SEQ_RegTask(1U << CFG_TASK_BLE_HOST, UTIL_SEQ_RFU, BleStack_Process_BG);
  UTIL_SEQ_RegTask(1U << CFG_TASK_HCI_ASYNCH_EVT_ID, UTIL_SEQ_RFU, Ble_UserEvtRx);

  /* NVM emulation in RAM initialization */
  NVM_Init(buffer_nvm, 0, CFG_BLEPLAT_NVM_MAX_SIZE);

  /* USER CODE BEGIN APP_BLE_Init_Buffers */

  /* USER CODE END APP_BLE_Init_Buffers */

  /* Check consistency */
  if (NVM_Get (NVM_FIRST, 0xFF, 0, 0, 0) != NVM_EOF)
  {
    NVM_Discard (NVM_ALL);
  }

  /* Initialize the BLE Host */
  if (HOST_BLE_Init() == 0u)
  {
    /* Initialization of HCI & GATT & GAP layer */
    Ble_Hci_Gap_Gatt_Init();

    /* Initialization of the BLE Services */
    SVCCTL_Init();

    /* Initialization of the BLE App Context */
    bleAppContext.Device_Connection_Status = APP_BLE_IDLE;
    bleAppContext.BleApplicationContext_legacy.connectionHandle = 0xFFFF;

    /* From here, all initialization are BLE application specific */

    /* USER CODE BEGIN APP_BLE_Init_4 */

    /* USER CODE END APP_BLE_Init_4 */

    /* Initialize Services and Characteristics. */
    LOG_INFO_APP("\n");
    LOG_INFO_APP("Services and Characteristics creation\n");
    A_APP_Init();
    LOG_INFO_APP("End of Services and Characteristics creation\n");
    LOG_INFO_APP("\n");

    /* USER CODE BEGIN APP_BLE_Init_3 */

    /* USER CODE END APP_BLE_Init_3 */

  }
  /* USER CODE BEGIN APP_BLE_Init_2 */
  
  /* EBYTE */
#if (APP_BLE_BRIDGE_RADIO_ENABLE != 0U)
  APP_BLE_Procedure_Gap_Peripheral(PROC_GAP_PERIPH_ADVERTISE_START_FAST);
  Bridge_StartScan();
#if (APP_BLE_BRIDGE_BINARY_DEBUG_FRAME_ENABLE != 0U)
  Bridge_UartSendFrame(APP_BLE_BRIDGE_UART_TYPE_LOCAL_STATE,
                       a_StatePayload,
                       APP_BLE_BRIDGE_STATE_PAYLOAD_LEN);
#endif
#else
  bridgeStarted = 0U;
#endif
  
  /* USER CODE END APP_BLE_Init_2 */

  return;
}

SVCCTL_UserEvtFlowStatus_t SVCCTL_App_Notification(void *p_Pckt)
{
  tBleStatus ret = BLE_STATUS_ERROR;
  hci_event_pckt    *p_event_pckt;
  evt_le_meta_event *p_meta_evt;
  evt_blecore_aci   *p_blecore_evt;

  p_event_pckt = (hci_event_pckt*) ((hci_uart_pckt *) p_Pckt)->data;
  UNUSED(ret);
  /* USER CODE BEGIN SVCCTL_App_Notification */

  /* USER CODE END SVCCTL_App_Notification */

  switch (p_event_pckt->evt)
  {
    case HCI_DISCONNECTION_COMPLETE_EVT_CODE:
    {
      hci_disconnection_complete_event_rp0 *p_disconnection_complete_event;
      p_disconnection_complete_event = (hci_disconnection_complete_event_rp0 *) p_event_pckt->data;
      if (p_disconnection_complete_event->Connection_Handle == bleAppContext.BleApplicationContext_legacy.connectionHandle)
      {
        bleAppContext.BleApplicationContext_legacy.connectionHandle = 0;
        bleAppContext.Device_Connection_Status = APP_BLE_IDLE;
        LOG_INFO_APP(">>== HCI_DISCONNECTION_COMPLETE_EVT_CODE\n");
        LOG_INFO_APP("     - Connection Handle:   0x%04X\n     - Reason:    0x%02X\n",
                    p_disconnection_complete_event->Connection_Handle,
                    p_disconnection_complete_event->Reason);

        /* USER CODE BEGIN EVT_DISCONN_COMPLETE_2 */

        /* USER CODE END EVT_DISCONN_COMPLETE_2 */
      }
      gap_cmd_resp_release();

      /* USER CODE BEGIN EVT_DISCONN_COMPLETE_1 */

      /* USER CODE END EVT_DISCONN_COMPLETE_1 */
      AHandleNotification.EvtOpcode = A_DISCON_HANDLE_EVT;
      AHandleNotification.ConnectionHandle = p_disconnection_complete_event->Connection_Handle;
      A_APP_EvtRx(&AHandleNotification);
      /* USER CODE BEGIN EVT_DISCONN_COMPLETE */

      /* USER CODE END EVT_DISCONN_COMPLETE */
      break; /* HCI_DISCONNECTION_COMPLETE_EVT_CODE */
    }
    case HCI_HARDWARE_ERROR_EVT_CODE:
    {
       hci_hardware_error_event_rp0 *p_hardware_error_event;

       p_hardware_error_event = (hci_hardware_error_event_rp0 *)p_event_pckt->data;
       UNUSED(p_hardware_error_event);
       APP_DBG_MSG(">>== HCI_HARDWARE_ERROR_EVT_CODE\n");
       APP_DBG_MSG("Hardware Code = 0x%02X\n",p_hardware_error_event->Hardware_Code);
       /* USER CODE BEGIN HCI_EVT_LE_HARDWARE_ERROR */

       /* USER CODE END HCI_EVT_LE_HARDWARE_ERROR */
       break; /* HCI_HARDWARE_ERROR_EVT_CODE */
    }
    case HCI_LE_META_EVT_CODE:
    {
      p_meta_evt = (evt_le_meta_event*) p_event_pckt->data;
      /* USER CODE BEGIN EVT_LE_META_EVENT */

      /* USER CODE END EVT_LE_META_EVENT */
      switch (p_meta_evt->subevent)
      {
        case HCI_LE_ADVERTISING_REPORT_SUBEVT_CODE:
        {
#if (APP_BLE_BRIDGE_RADIO_ENABLE != 0U)
          Bridge_HandleRawAdvertisingReports(p_meta_evt->data);
#endif
          break;
        }
        case HCI_LE_CONNECTION_UPDATE_COMPLETE_SUBEVT_CODE:
        {
          uint32_t conn_interval_us = 0;
          hci_le_connection_update_complete_event_rp0 *p_conn_update_complete;
          p_conn_update_complete = (hci_le_connection_update_complete_event_rp0 *) p_meta_evt->data;
          conn_interval_us = p_conn_update_complete->Conn_Interval * 1250;
          LOG_INFO_APP(">>== HCI_LE_CONNECTION_UPDATE_COMPLETE_SUBEVT_CODE\n");
          LOG_INFO_APP("     - Connection Interval:   %d.%02d ms\n     - Connection latency:    %d\n     - Supervision Timeout:   %d ms\n",
                       conn_interval_us / 1000,
                       (conn_interval_us%1000) / 10,
                       p_conn_update_complete->Conn_Latency,
                       p_conn_update_complete->Supervision_Timeout*10);
          UNUSED(conn_interval_us);
          UNUSED(p_conn_update_complete);

          /* USER CODE BEGIN EVT_LE_CONN_UPDATE_COMPLETE */

          /* USER CODE END EVT_LE_CONN_UPDATE_COMPLETE */
          break;
        }
        case HCI_LE_PHY_UPDATE_COMPLETE_SUBEVT_CODE:
        {
          hci_le_phy_update_complete_event_rp0 *p_le_phy_update_complete;
          p_le_phy_update_complete = (hci_le_phy_update_complete_event_rp0*)p_meta_evt->data;
          UNUSED(p_le_phy_update_complete);

          gap_cmd_resp_release();

          /* USER CODE BEGIN EVT_LE_PHY_UPDATE_COMPLETE */

          /* USER CODE END EVT_LE_PHY_UPDATE_COMPLETE */
          break;
        }
        case HCI_LE_ENHANCED_CONNECTION_COMPLETE_SUBEVT_CODE:
        {
          uint32_t conn_interval_us = 0;
          hci_le_enhanced_connection_complete_event_rp0 *p_enhanced_conn_complete;
          p_enhanced_conn_complete = (hci_le_enhanced_connection_complete_event_rp0 *) p_meta_evt->data;
          conn_interval_us = p_enhanced_conn_complete->Conn_Interval * 1250;
          LOG_INFO_APP(">>== HCI_LE_ENHANCED_CONNECTION_COMPLETE_SUBEVT_CODE - Connection handle: 0x%04X\n", p_enhanced_conn_complete->Connection_Handle);
          LOG_INFO_APP("     - Connection established with @:%02x:%02x:%02x:%02x:%02x:%02x\n",
                      p_enhanced_conn_complete->Peer_Address[5],
                      p_enhanced_conn_complete->Peer_Address[4],
                      p_enhanced_conn_complete->Peer_Address[3],
                      p_enhanced_conn_complete->Peer_Address[2],
                      p_enhanced_conn_complete->Peer_Address[1],
                      p_enhanced_conn_complete->Peer_Address[0]);
          LOG_INFO_APP("     - Connection Interval:   %d.%02d ms\n     - Connection latency:    %d\n     - Supervision Timeout:   %d ms\n",
                      conn_interval_us / 1000,
                      (conn_interval_us%1000) / 10,
                      p_enhanced_conn_complete->Conn_Latency,
                      p_enhanced_conn_complete->Supervision_Timeout * 10
                     );
          UNUSED(conn_interval_us);

          if (bleAppContext.Device_Connection_Status == APP_BLE_LP_CONNECTING)
          {
            /* Connection as client */
            bleAppContext.Device_Connection_Status = APP_BLE_CONNECTED_CLIENT;
          }
          else
          {
            /* Connection as server */
            bleAppContext.Device_Connection_Status = APP_BLE_CONNECTED_SERVER;
          }
          bleAppContext.BleApplicationContext_legacy.connectionHandle = p_enhanced_conn_complete->Connection_Handle;

          AHandleNotification.EvtOpcode = A_CONN_HANDLE_EVT;
          AHandleNotification.ConnectionHandle = p_enhanced_conn_complete->Connection_Handle;
          A_APP_EvtRx(&AHandleNotification);
          /* USER CODE BEGIN HCI_EVT_LE_ENHANCED_CONN_COMPLETE */

          /* USER CODE END HCI_EVT_LE_ENHANCED_CONN_COMPLETE */
          break; /* HCI_LE_ENHANCED_CONNECTION_COMPLETE_SUBEVT_CODE */
        }
        case HCI_LE_CONNECTION_COMPLETE_SUBEVT_CODE:
        {
          uint32_t conn_interval_us = 0;
          hci_le_connection_complete_event_rp0 *p_conn_complete;
          p_conn_complete = (hci_le_connection_complete_event_rp0 *) p_meta_evt->data;
          conn_interval_us = p_conn_complete->Conn_Interval * 1250;
          LOG_INFO_APP(">>== HCI_LE_CONNECTION_COMPLETE_SUBEVT_CODE - Connection handle: 0x%04X\n", p_conn_complete->Connection_Handle);
          LOG_INFO_APP("     - Connection established with @:%02x:%02x:%02x:%02x:%02x:%02x\n",
                      p_conn_complete->Peer_Address[5],
                      p_conn_complete->Peer_Address[4],
                      p_conn_complete->Peer_Address[3],
                      p_conn_complete->Peer_Address[2],
                      p_conn_complete->Peer_Address[1],
                      p_conn_complete->Peer_Address[0]);
          LOG_INFO_APP("     - Connection Interval:   %d.%02d ms\n     - Connection latency:    %d\n     - Supervision Timeout:   %d ms\n",
                      conn_interval_us / 1000,
                      (conn_interval_us%1000) / 10,
                      p_conn_complete->Conn_Latency,
                      p_conn_complete->Supervision_Timeout * 10
                     );
          UNUSED(conn_interval_us);

          if (bleAppContext.Device_Connection_Status == APP_BLE_LP_CONNECTING)
          {
            /* Connection as client */
            bleAppContext.Device_Connection_Status = APP_BLE_CONNECTED_CLIENT;
          }
          else
          {
            /* Connection as server */
            bleAppContext.Device_Connection_Status = APP_BLE_CONNECTED_SERVER;
          }
          bleAppContext.BleApplicationContext_legacy.connectionHandle = p_conn_complete->Connection_Handle;

          AHandleNotification.EvtOpcode = A_CONN_HANDLE_EVT;
          AHandleNotification.ConnectionHandle = p_conn_complete->Connection_Handle;
          A_APP_EvtRx(&AHandleNotification);
          /* USER CODE BEGIN HCI_EVT_LE_CONN_COMPLETE */

          /* USER CODE END HCI_EVT_LE_CONN_COMPLETE */
          break; /* HCI_LE_CONNECTION_COMPLETE_SUBEVT_CODE */
        }
        /* USER CODE BEGIN SUBEVENT */

        /* USER CODE END SUBEVENT */
        default:
        {
          /* USER CODE BEGIN SUBEVENT_DEFAULT */

          /* USER CODE END SUBEVENT_DEFAULT */
          break;
        }
      }

      /* USER CODE BEGIN META_EVT */

      /* USER CODE END META_EVT */
    }
    break; /* HCI_LE_META_EVT_CODE */

    case HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE:
    {
      p_blecore_evt = (evt_blecore_aci*) p_event_pckt->data;
      /* USER CODE BEGIN EVT_VENDOR */

      /* USER CODE END EVT_VENDOR */
      switch (p_blecore_evt->ecode)
      {
        /* USER CODE BEGIN ECODE */

        /* USER CODE END ECODE */
        case ACI_L2CAP_CONNECTION_UPDATE_RESP_VSEVT_CODE:
        {
          aci_l2cap_connection_update_resp_event_rp0 *p_l2cap_conn_update_resp;
          p_l2cap_conn_update_resp = (aci_l2cap_connection_update_resp_event_rp0 *) p_blecore_evt->data;
          UNUSED(p_l2cap_conn_update_resp);
          /* USER CODE BEGIN EVT_L2CAP_CONNECTION_UPDATE_RESP */

          /* USER CODE END EVT_L2CAP_CONNECTION_UPDATE_RESP */
          break;
        }
        case ACI_GAP_PROC_COMPLETE_VSEVT_CODE:
        {
          aci_gap_proc_complete_event_rp0 *p_gap_proc_complete;
          p_gap_proc_complete = (aci_gap_proc_complete_event_rp0*) p_blecore_evt->data;
          UNUSED(p_gap_proc_complete);

          LOG_INFO_APP(">>== ACI_GAP_PROC_COMPLETE_VSEVT_CODE\n");
          /* USER CODE BEGIN EVT_GAP_PROCEDURE_COMPLETE */

          /* USER CODE END EVT_GAP_PROCEDURE_COMPLETE */
          break; /* ACI_GAP_PROC_COMPLETE_VSEVT_CODE */
        }
        case ACI_HAL_END_OF_RADIO_ACTIVITY_VSEVT_CODE:
        {
          /* USER CODE BEGIN RADIO_ACTIVITY_EVENT */

          /* USER CODE END RADIO_ACTIVITY_EVENT */
          break; /* ACI_HAL_END_OF_RADIO_ACTIVITY_VSEVT_CODE */
        }
        case ACI_GAP_KEYPRESS_NOTIFICATION_VSEVT_CODE:
        {
          LOG_INFO_APP(">>== ACI_GAP_KEYPRESS_NOTIFICATION_VSEVT_CODE\n");
          /* USER CODE BEGIN ACI_GAP_KEYPRESS_NOTIFICATION_VSEVT_CODE */

          /* USER CODE END ACI_GAP_KEYPRESS_NOTIFICATION_VSEVT_CODE */
          break;
        }
        case ACI_GAP_PASS_KEY_REQ_VSEVT_CODE:
        {
          uint32_t pin;
          LOG_INFO_APP(">>== ACI_GAP_PASS_KEY_REQ_VSEVT_CODE\n");

          pin = CFG_FIXED_PIN;
          /* USER CODE BEGIN ACI_GAP_PASS_KEY_REQ_VSEVT_CODE_0 */

          /* USER CODE END ACI_GAP_PASS_KEY_REQ_VSEVT_CODE_0 */

          ret = aci_gap_pass_key_resp(bleAppContext.BleApplicationContext_legacy.connectionHandle, pin);
          if (ret != BLE_STATUS_SUCCESS)
          {
            LOG_INFO_APP("==>> aci_gap_pass_key_resp : Fail, reason: 0x%02X\n", ret);
          }
          else
          {
            LOG_INFO_APP("==>> aci_gap_pass_key_resp : Success\n");
          }
          /* USER CODE BEGIN ACI_GAP_PASS_KEY_REQ_VSEVT_CODE */

          /* USER CODE END ACI_GAP_PASS_KEY_REQ_VSEVT_CODE */
          break;
        }
        case ACI_GAP_NUMERIC_COMPARISON_VALUE_VSEVT_CODE:
        {
          uint8_t confirm_value;
          LOG_INFO_APP(">>== ACI_GAP_NUMERIC_COMPARISON_VALUE_VSEVT_CODE\n");
          LOG_INFO_APP("     - numeric_value = %ld\n",
                      ((aci_gap_numeric_comparison_value_event_rp0 *)(p_blecore_evt->data))->Numeric_Value);
          LOG_INFO_APP("     - Hex_value = %lx\n",
                      ((aci_gap_numeric_comparison_value_event_rp0 *)(p_blecore_evt->data))->Numeric_Value);

          /* Set confirm value to 1(YES) */
          confirm_value = 1;
          /* USER CODE BEGIN ACI_GAP_NUMERIC_COMPARISON_VALUE_VSEVT_CODE_0 */

          /* USER CODE END ACI_GAP_NUMERIC_COMPARISON_VALUE_VSEVT_CODE_0 */

          ret = aci_gap_numeric_comparison_value_confirm_yesno(bleAppContext.BleApplicationContext_legacy.connectionHandle, confirm_value);
          if (ret != BLE_STATUS_SUCCESS)
          {
            LOG_INFO_APP("==>> aci_gap_numeric_comparison_value_confirm_yesno : Fail, reason: 0x%02X\n", ret);
          }
          else
          {
            LOG_INFO_APP("==>> aci_gap_numeric_comparison_value_confirm_yesno : Success\n");
          }
          /* USER CODE BEGIN ACI_GAP_NUMERIC_COMPARISON_VALUE_VSEVT_CODE */

          /* USER CODE END ACI_GAP_NUMERIC_COMPARISON_VALUE_VSEVT_CODE */
          break;
        }
        case ACI_GAP_PAIRING_COMPLETE_VSEVT_CODE:
        {
          LOG_INFO_APP(">>== ACI_GAP_PAIRING_COMPLETE_VSEVT_CODE\n");
          aci_gap_pairing_complete_event_rp0 *p_pairing_complete;
          p_pairing_complete = (aci_gap_pairing_complete_event_rp0*)p_blecore_evt->data;

          if (p_pairing_complete->Status != 0)
          {
            LOG_INFO_APP("     - Pairing KO\n     - Status: 0x%02X\n     - Reason: 0x%02X\n",
                         p_pairing_complete->Status, p_pairing_complete->Reason);
          }
          else
          {
            LOG_INFO_APP("     - Pairing Success\n");
          }
          LOG_INFO_APP("\n");

          /* USER CODE BEGIN ACI_GAP_PAIRING_COMPLETE_VSEVT_CODE */

          /* USER CODE END ACI_GAP_PAIRING_COMPLETE_VSEVT_CODE */
          break;
        }
        case ACI_GAP_BOND_LOST_VSEVT_CODE:
        {
          LOG_INFO_APP(">>== ACI_GAP_BOND_LOST_EVENT\n");
          ret = aci_gap_allow_rebond(bleAppContext.BleApplicationContext_legacy.connectionHandle);
          if (ret != BLE_STATUS_SUCCESS)
          {
            LOG_INFO_APP("==>> aci_gap_allow_rebond : Fail, reason: 0x%02X\n", ret);
          }
          else
          {
            LOG_INFO_APP("==>> aci_gap_allow_rebond : Success\n");
          }
          /* USER CODE BEGIN ACI_GAP_BOND_LOST_VSEVT_CODE */

          /* USER CODE END ACI_GAP_BOND_LOST_VSEVT_CODE */
          break;
        }
        case ACI_HAL_FW_ERROR_VSEVT_CODE:
        {
          aci_hal_fw_error_event_rp0 *p_fw_error_event;

          p_fw_error_event = (aci_hal_fw_error_event_rp0 *)p_blecore_evt->data;
          UNUSED(p_fw_error_event);
          APP_DBG_MSG(">>== ACI_HAL_FW_ERROR_VSEVT_CODE\n");
          APP_DBG_MSG("FW Error Type = 0x%02X\n", p_fw_error_event->FW_Error_Type);
          /* USER CODE BEGIN ACI_HAL_FW_ERROR_VSEVT_CODE */

          /* USER CODE END ACI_HAL_FW_ERROR_VSEVT_CODE */
          break;
        }
        /* USER CODE BEGIN ECODE_1 */

        /* USER CODE END ECODE_1 */
        default:
        {
          /* USER CODE BEGIN ECODE_DEFAULT */

          /* USER CODE END ECODE_DEFAULT */
          break;
        }
      }
      /* USER CODE BEGIN EVT_VENDOR_1 */

      /* USER CODE END EVT_VENDOR_1 */
    }
    break; /* HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE */
    /* USER CODE BEGIN EVENT_PCKT */

    /* USER CODE END EVENT_PCKT */
    default:
    {
      /* USER CODE BEGIN EVENT_PCKT_DEFAULT */

      /* USER CODE END EVENT_PCKT_DEFAULT */
      break;
    }
  }
  /* USER CODE BEGIN SVCCTL_App_Notification_1 */

  /* USER CODE END SVCCTL_App_Notification_1 */

  return (SVCCTL_UserEvtFlowEnable);
}

APP_BLE_ConnStatus_t APP_BLE_Get_Server_Connection_Status(void)
{
  return bleAppContext.Device_Connection_Status;
}

void APP_BLE_Procedure_Gap_General(ProcGapGeneralId_t ProcGapGeneralId)
{
  tBleStatus status;

  /* USER CODE BEGIN Procedure_Gap_General_1 */

  /* USER CODE END Procedure_Gap_General_1 */

  switch(ProcGapGeneralId)
  {
    case PROC_GAP_GEN_PHY_TOGGLE:
    {
      uint8_t phy_tx = 0U, phy_rx = 0U;

      status = hci_le_read_phy(bleAppContext.BleApplicationContext_legacy.connectionHandle, &phy_tx, &phy_rx);

      if (status != BLE_STATUS_SUCCESS)
      {
        LOG_INFO_APP("hci_le_read_phy failure: reason=0x%02X\n",status);
      }
      else
      {
        LOG_INFO_APP("==>> hci_le_read_phy - Success\n");
        LOG_INFO_APP("==>> PHY Param  TX= %d, RX= %d\n", phy_tx, phy_rx);
        if ((phy_tx == HCI_TX_PHY_LE_2M) && (phy_rx == HCI_RX_PHY_LE_2M))
        {
          LOG_INFO_APP("==>> hci_le_set_phy PHY Param  TX= %d, RX= %d - ", HCI_TX_PHY_LE_1M, HCI_RX_PHY_LE_1M);
          status = hci_le_set_phy(bleAppContext.BleApplicationContext_legacy.connectionHandle, 0, HCI_TX_PHYS_LE_1M_PREF, HCI_RX_PHYS_LE_1M_PREF, 0);
          if (status != BLE_STATUS_SUCCESS)
          {
            LOG_INFO_APP("Fail\n");
          }
          else
          {
            LOG_INFO_APP("Success\n");
            gap_cmd_resp_wait();/* waiting for HCI_LE_PHY_UPDATE_COMPLETE_SUBEVT_CODE */
          }
        }
        else
        {
          LOG_INFO_APP("==>> hci_le_set_phy PHY Param  TX= %d, RX= %d - ", HCI_TX_PHYS_LE_2M_PREF, HCI_RX_PHYS_LE_2M_PREF);
          status = hci_le_set_phy(bleAppContext.BleApplicationContext_legacy.connectionHandle, 0, HCI_TX_PHYS_LE_2M_PREF, HCI_RX_PHYS_LE_2M_PREF, 0);
          if (status != BLE_STATUS_SUCCESS)
          {
            LOG_INFO_APP("Fail\n");
          }
          else
          {
            LOG_INFO_APP("Success\n");
            gap_cmd_resp_wait();/* waiting for HCI_LE_PHY_UPDATE_COMPLETE_SUBEVT_CODE */
          }
        }
      }
      break;
    }/* PROC_GAP_GEN_PHY_TOGGLE */
    case PROC_GAP_GEN_CONN_TERMINATE:
    {
      status = aci_gap_terminate(bleAppContext.BleApplicationContext_legacy.connectionHandle, HCI_REMOTE_USER_TERMINATED_CONNECTION_ERR_CODE);
      if (status != BLE_STATUS_SUCCESS)
      {
         LOG_INFO_APP("aci_gap_terminate failure: reason=0x%02X\n", status);
      }
      else
      {
        LOG_INFO_APP("==>> aci_gap_terminate : Success\n");
      }
      gap_cmd_resp_wait();/* waiting for HCI_DISCONNECTION_COMPLETE_EVT_CODE */
      break;
    }/* PROC_GAP_GEN_CONN_TERMINATE */
    case PROC_GATT_EXCHANGE_CONFIG:
    {
      status = aci_gatt_exchange_config(bleAppContext.BleApplicationContext_legacy.connectionHandle);
      if (status != BLE_STATUS_SUCCESS)
      {
        LOG_INFO_APP("aci_gatt_exchange_config failure: reason=0x%02X\n", status);
      }
      else
      {
        LOG_INFO_APP("==>> aci_gatt_exchange_config : Success\n");
      }
      break;
    }
    /* USER CODE BEGIN GAP_GENERAL */

    /* USER CODE END GAP_GENERAL */
    default:
      break;
  }

  /* USER CODE BEGIN Procedure_Gap_General_2 */

  /* USER CODE END Procedure_Gap_General_2 */
  return;
}

void APP_BLE_Procedure_Gap_Peripheral(ProcGapPeripheralId_t ProcGapPeripheralId)
{
  tBleStatus status;
  uint32_t paramA = 0U;
  uint32_t paramB = 0U;
  uint32_t paramC = 0U;
  uint32_t paramD = 0U;

  /* USER CODE BEGIN Procedure_Gap_Peripheral_1 */

  /* USER CODE END Procedure_Gap_Peripheral_1 */

  /* First set parameters before calling ACI APIs, only if needed */
  switch(ProcGapPeripheralId)
  {
    case PROC_GAP_PERIPH_ADVERTISE_START_FAST:
    {
      paramA = ADV_INTERVAL_MIN;
      paramB = ADV_INTERVAL_MAX;
      paramC = APP_BLE_ADV_FAST;
      paramD = ADV_IND;

      break;
    }
    case PROC_GAP_PERIPH_ADVERTISE_NON_CONN_START_FAST:
    {
      paramA = ADV_INTERVAL_MIN;
      paramB = ADV_INTERVAL_MAX;
      paramC = APP_BLE_ADV_NON_CONN_FAST;
      paramD = ADV_NONCONN_IND;

      break;
    }
    case PROC_GAP_PERIPH_ADVERTISE_START_LP:
    {
      paramA = ADV_LP_INTERVAL_MIN;
      paramB = ADV_LP_INTERVAL_MAX;
      paramC = APP_BLE_ADV_LP;
      paramD = ADV_IND;

      break;
    }
    case PROC_GAP_PERIPH_ADVERTISE_NON_CONN_START_LP:
    {
      paramA = ADV_LP_INTERVAL_MIN;
      paramB = ADV_LP_INTERVAL_MAX;
      paramC = APP_BLE_ADV_NON_CONN_LP;
      paramD = ADV_NONCONN_IND;

      break;
    }
    case PROC_GAP_PERIPH_ADVERTISE_STOP:
    {
      paramC = APP_BLE_IDLE;

      break;
    }/* PROC_GAP_PERIPH_ADVERTISE_STOP */
    case PROC_GAP_PERIPH_CONN_PARAM_UPDATE:
    {
      paramA = CONN_INT_MS(1000);
      paramB = CONN_INT_MS(1000);
      paramC = 0x0000;
      paramD = 0x01F4;

      /* USER CODE BEGIN CONN_PARAM_UPDATE */

      /* USER CODE END CONN_PARAM_UPDATE */
      break;
    }/* PROC_GAP_PERIPH_CONN_PARAM_UPDATE */
    /* USER CODE BEGIN PARAM_UPDATE_1 */

    /* USER CODE END PARAM_UPDATE_1 */
    default:
      break;
  }

  /* USER CODE BEGIN Procedure_Gap_Peripheral_2 */

  /* USER CODE END Procedure_Gap_Peripheral_2 */

  /* Call ACI APIs */
  switch(ProcGapPeripheralId)
  {
    case PROC_GAP_PERIPH_ADVERTISE_START_FAST:
    case PROC_GAP_PERIPH_ADVERTISE_START_LP:
    case PROC_GAP_PERIPH_ADVERTISE_NON_CONN_START_FAST:
    case PROC_GAP_PERIPH_ADVERTISE_NON_CONN_START_LP:
    {
      /* Start Advertising */
      status = aci_gap_set_discoverable(paramD,
                                        paramA,
                                        paramB,
                                        CFG_BD_ADDRESS_TYPE,
                                        ADV_FILTER,
                                        0, 0, 0, 0, 0, 0);
      if (status != BLE_STATUS_SUCCESS)
      {
        LOG_INFO_APP("==>> aci_gap_set_discoverable - fail, result: 0x%02X\n", status);
      }
      else
      {
        bleAppContext.Device_Connection_Status = (APP_BLE_ConnStatus_t)paramC;
        LOG_INFO_APP("==>> aci_gap_set_discoverable - Success\n");
      }

      status = aci_gap_delete_ad_type(AD_TYPE_TX_POWER_LEVEL);
      if (status != BLE_STATUS_SUCCESS)
      {
        LOG_INFO_APP("==>> delete tx power level - fail, result: 0x%02X\n", status);
      }

      /* Update Advertising data */
      uint8_t *adv_data_p;
      uint8_t adv_data_len;

      adv_data_p = &a_AdvData[0];
      adv_data_len = a_AdvDataLen;
      /* USER CODE BEGIN ADV_DATA_UPDATE_1 */
      Bridge_UpdateAdvertisingData();
      adv_data_len = a_AdvDataLen;
      (void)hci_le_set_scan_response_data(a_ScanRspDataLen, a_ScanRspData);

      /* USER CODE END ADV_DATA_UPDATE_1 */
      status = aci_gap_update_adv_data(adv_data_len, adv_data_p);
      if (status != BLE_STATUS_SUCCESS)
      {
        LOG_INFO_APP("==>> Start Advertising Failed, result: 0x%02X\n", status);
      }
      else
      {
        LOG_INFO_APP("==>> Success: Start Advertising\n");
      }
      break;
    }
    case PROC_GAP_PERIPH_ADVERTISE_STOP:
    {
      status = aci_gap_set_non_discoverable();
      if (status != BLE_STATUS_SUCCESS)
      {
        LOG_INFO_APP("aci_gap_set_non_discoverable - fail, result: 0x%02X\n",status);
      }
      else
      {
        bleAppContext.Device_Connection_Status = (APP_BLE_ConnStatus_t)paramC;
        LOG_INFO_APP("==>> aci_gap_set_non_discoverable - Success\n");
      }
      break;
    }/* PROC_GAP_PERIPH_ADVERTISE_STOP */
    case PROC_GAP_PERIPH_ADVERTISE_DATA_UPDATE:
    {
      uint8_t *adv_data_p;
      uint8_t adv_data_len;

      adv_data_p = &a_AdvData[0];
      adv_data_len = a_AdvDataLen;
      /* USER CODE BEGIN ADV_DATA_UPDATE_2 */
      Bridge_UpdateAdvertisingData();
      adv_data_len = a_AdvDataLen;
      (void)hci_le_set_scan_response_data(a_ScanRspDataLen, a_ScanRspData);

      /* USER CODE END ADV_DATA_UPDATE_2 */
      status = aci_gap_update_adv_data(adv_data_len, adv_data_p);
      if (status != BLE_STATUS_SUCCESS)
      {
        LOG_INFO_APP("aci_gap_update_adv_data - fail, result: 0x%02X\n",status);
      }
      else
      {
        LOG_INFO_APP("==>> aci_gap_update_adv_data - Success\n");
      }

      break;
    }/* PROC_GAP_PERIPH_ADVERTISE_DATA_UPDATE */
    case PROC_GAP_PERIPH_CONN_PARAM_UPDATE:
    {
       status = aci_l2cap_connection_parameter_update_req(
                                                       bleAppContext.BleApplicationContext_legacy.connectionHandle,
                                                       paramA,
                                                       paramB,
                                                       paramC,
                                                       paramD);
      if (status != BLE_STATUS_SUCCESS)
      {
        LOG_INFO_APP("aci_l2cap_connection_parameter_update_req - fail, result: 0x%02X\n",status);
      }
      else
      {
        LOG_INFO_APP("==>> aci_l2cap_connection_parameter_update_req - Success\n");
      }

      break;
    }/* PROC_GAP_PERIPH_CONN_PARAM_UPDATE */
    /* USER CODE BEGIN ACI_CALL */

    /* USER CODE END ACI_CALL */
    default:
      break;
  }

  /* USER CODE BEGIN Procedure_Gap_Peripheral_3 */

  /* USER CODE END Procedure_Gap_Peripheral_3 */
  return;
}

const uint8_t* BleGetBdAddress(void)
{
  const uint8_t *p_bd_addr;

  p_bd_addr = (const uint8_t *)a_BdAddr;

  return p_bd_addr;
}

/* USER CODE BEGIN FD */
void APP_BLE_UartRxData(const uint8_t *pData, uint16_t len)
{
  if ((pData == NULL) || (len == 0U))
  {
    return;
  }

  if (len > sizeof(a_UartRxBuf))
  {
    pData += (len - sizeof(a_UartRxBuf));
    len = sizeof(a_UartRxBuf);
    uartRxLen = 0U;
  }

  if ((uartRxLen + len) > sizeof(a_UartRxBuf))
  {
    uartRxLen = 0U;
  }

  memcpy(&a_UartRxBuf[uartRxLen], pData, len);
  uartRxLen += len;
  Bridge_UartProcessRx();
}

void APP_BLE_Protocol_Tick(void)
{
  uint32_t now = HAL_GetTick();

  if ((now - lastAdvRefreshTick) >= APP_BLE_BRIDGE_ADV_REFRESH_MS)
  {
    lastAdvRefreshTick = now;
#if (APP_BLE_BRIDGE_RADIO_ENABLE != 0U)
    if (bridgeStarted != 0U)
    {
      APP_BLE_Procedure_Gap_Peripheral(PROC_GAP_PERIPH_ADVERTISE_DATA_UPDATE);
    }
#else
    Bridge_UpdateAdvertisingData();
#endif
  }
  Bridge_CheckPeerTimeout();
}

uint8_t APP_BLE_TakeVoiceWarning(APP_BLE_VoiceWarning_t *warning)
{
  if ((warning == NULL) || (pendingVoiceWarning.prompt == APP_BLE_VOICE_NONE))
  {
    return 0U;
  }

  *warning = pendingVoiceWarning;
  pendingVoiceWarning.prompt = APP_BLE_VOICE_NONE;
  bridgeVoiceReportValid = 1U;
  bridgeVoiceLastReportMs = HAL_GetTick();
  bridgeVoiceClearPending = 0U;
  bridgeVoiceClearStartMs = 0U;
  Bridge_ResetVoiceCandidate();
  return 1U;
}

tBleStatus hci_le_advertising_report_event(uint8_t Num_Reports,
                                            const Advertising_Report_t *Advertising_Report)
{
#if (APP_BLE_BRIDGE_RADIO_ENABLE != 0U)
  uint8_t payload[APP_BLE_BRIDGE_STATE_PAYLOAD_LEN];

  for (uint8_t i = 0U; i < Num_Reports; i++)
  {
    if (Bridge_FilterAdvertisingReport(&Advertising_Report[i], payload) != 0U)
    {
      Bridge_HandlePeerPayload(payload, Advertising_Report[i].RSSI, 1U);
#if (APP_BLE_BRIDGE_BINARY_DEBUG_FRAME_ENABLE != 0U)
      Bridge_UartSendFrame(APP_BLE_BRIDGE_UART_TYPE_SCAN_STATE,
                           payload,
                           APP_BLE_BRIDGE_STATE_PAYLOAD_LEN);
#endif
    }
  }
#else
  UNUSED(Num_Reports);
  UNUSED(Advertising_Report);
#endif

  return HCI_SUCCESS_ERR_CODE;
}

/* USER CODE END FD */

/*************************************************************
 *
 * LOCAL FUNCTIONS
 *
 *************************************************************/
static uint8_t HOST_BLE_Init(void)
{
  tBleStatus return_status;

  pInitParams.numAttrRecord           = CFG_BLE_NUM_GATT_ATTRIBUTES;
  pInitParams.numAttrServ             = CFG_BLE_NUM_GATT_SERVICES;
  pInitParams.attrValueArrSize        = CFG_BLE_ATT_VALUE_ARRAY_SIZE;
  pInitParams.prWriteListSize         = CFG_BLE_ATTR_PREPARE_WRITE_VALUE_SIZE;
  pInitParams.attMtu                  = CFG_BLE_ATT_MTU_MAX;
  pInitParams.max_coc_nbr             = CFG_BLE_COC_NBR_MAX;
  pInitParams.max_coc_mps             = CFG_BLE_COC_MPS_MAX;
  pInitParams.max_coc_initiator_nbr   = CFG_BLE_COC_INITIATOR_NBR_MAX;
  pInitParams.numOfLinks              = CFG_BLE_NUM_LINK;
  pInitParams.mblockCount             = CFG_BLE_MBLOCK_COUNT;
  pInitParams.bleStartRamAddress      = (uint8_t*)buffer;
  pInitParams.total_buffer_size       = BLE_DYN_ALLOC_SIZE;
  pInitParams.bleStartRamAddress_GATT = (uint8_t*)gatt_buffer;
  pInitParams.total_buffer_size_GATT  = BLE_GATT_BUF_SIZE;
  pInitParams.options                 = CFG_BLE_OPTIONS;
  pInitParams.debug                   = 0U;
/* USER CODE BEGIN HOST_BLE_Init_Params */

/* USER CODE END HOST_BLE_Init_Params */
  return_status = BleStack_Init(&pInitParams);
/* USER CODE BEGIN HOST_BLE_Init */

/* USER CODE END HOST_BLE_Init */
  return ((uint8_t)return_status);
}

static void Ble_Hci_Gap_Gatt_Init(void)
{
  uint8_t role;
  uint16_t gap_service_handle = 0U, gap_dev_name_char_handle = 0U, gap_appearance_char_handle = 0U;
  const uint8_t *p_bd_addr;
  uint16_t a_appearance[1] = {CFG_GAP_APPEARANCE};
  const uint8_t *p_ir_value;
  const uint8_t *p_er_value;
  tBleStatus ret;

  /* USER CODE BEGIN Ble_Hci_Gap_Gatt_Init */

  /* USER CODE END Ble_Hci_Gap_Gatt_Init */

  LOG_INFO_APP("==>> Start Ble_Hci_Gap_Gatt_Init function\n");

  /* Write the BD Address */
  p_bd_addr = BleGenerateBdAddress();

  /* USER CODE BEGIN BD_Address_Mngt */

  /* USER CODE END BD_Address_Mngt */

  ret = aci_hal_write_config_data(CONFIG_DATA_PUBADDR_OFFSET,
                                  CONFIG_DATA_PUBADDR_LEN,
                                  (uint8_t*) p_bd_addr);
  if (ret != BLE_STATUS_SUCCESS)
  {
    LOG_INFO_APP("  Fail   : aci_hal_write_config_data command - CONFIG_DATA_PUBADDR_OFFSET, result: 0x%02X\n", ret);
  }
  else
  {
    LOG_INFO_APP("  Success: aci_hal_write_config_data command - CONFIG_DATA_PUBADDR_OFFSET\n");
    LOG_INFO_APP("  Public Bluetooth Address: %02x:%02x:%02x:%02x:%02x:%02x\n",p_bd_addr[5],p_bd_addr[4],p_bd_addr[3],p_bd_addr[2],p_bd_addr[1],p_bd_addr[0]);
  }

  /* Generate Identity root key Value */
  p_ir_value = BleGenerateIRValue();

  /* Write Identity root key used to derive IRK and DHK(Legacy) */
  ret = aci_hal_write_config_data(CONFIG_DATA_IR_OFFSET, CONFIG_DATA_IR_LEN, p_ir_value);
  if (ret != BLE_STATUS_SUCCESS)
  {
    LOG_INFO_APP("  Fail   : aci_hal_write_config_data command - CONFIG_DATA_IR_OFFSET, result: 0x%02X\n", ret);
  }
  else
  {
    LOG_INFO_APP("  Success: aci_hal_write_config_data command - CONFIG_DATA_IR_OFFSET\n");
  }

  /* Generate Encryption root key Value */
  p_er_value = BleGenerateERValue();

  /* Write Encryption root key used to derive LTK and CSRK */
  ret = aci_hal_write_config_data(CONFIG_DATA_ER_OFFSET, CONFIG_DATA_ER_LEN, p_er_value);
  if (ret != BLE_STATUS_SUCCESS)
  {
    LOG_INFO_APP("  Fail   : aci_hal_write_config_data command - CONFIG_DATA_ER_OFFSET, result: 0x%02X\n", ret);
  }
  else
  {
    LOG_INFO_APP("  Success: aci_hal_write_config_data command - CONFIG_DATA_ER_OFFSET\n");
  }

  /* Set Transmission RF Power. */
  ret = aci_hal_set_tx_power_level(1, CFG_TX_POWER);
  if (ret != BLE_STATUS_SUCCESS)
  {
    LOG_INFO_APP("  Fail   : aci_hal_set_tx_power_level command, result: 0x%02X\n", ret);
  }
  else
  {
    LOG_INFO_APP("  Success: aci_hal_set_tx_power_level command\n");
  }

  /* Initialize GATT interface */
  ret = aci_gatt_init();
  if (ret != BLE_STATUS_SUCCESS)
  {
    LOG_INFO_APP("  Fail   : aci_gatt_init command, result: 0x%02X\n", ret);
  }
  else
  {
    LOG_INFO_APP("  Success: aci_gatt_init command\n");
  }

  /* Initialize GAP interface */
  role = 0U;
  role |= GAP_PERIPHERAL_ROLE;

  /* USER CODE BEGIN Role_Mngt */
  role |= GAP_OBSERVER_ROLE;

  /* USER CODE END Role_Mngt */

  if (role > 0)
  {
    ret = aci_gap_init(role,
                       CFG_PRIVACY,
                       sizeof(a_GapDeviceName),
                       &gap_service_handle,
                       &gap_dev_name_char_handle,
                       &gap_appearance_char_handle);

    if (ret != BLE_STATUS_SUCCESS)
    {
      LOG_INFO_APP("  Fail   : aci_gap_init command, result: 0x%02X\n", ret);
    }
    else
    {
      LOG_INFO_APP("  Success: aci_gap_init command\n");
    }

    ret = aci_gatt_update_char_value(gap_service_handle,
                                     gap_dev_name_char_handle,
                                     0,
                                     sizeof(a_GapDeviceName),
                                     (uint8_t *) a_GapDeviceName);
    if (ret != BLE_STATUS_SUCCESS)
    {
      LOG_INFO_APP("  Fail   : aci_gatt_update_char_value - Device Name, result: 0x%02X\n", ret);
    }
    else
    {
      LOG_INFO_APP("  Success: aci_gatt_update_char_value - Device Name\n");
    }

    ret = aci_gatt_update_char_value(gap_service_handle,
                                     gap_appearance_char_handle,
                                     0,
                                     sizeof(a_appearance),
                                     (uint8_t *)&a_appearance);
    if (ret != BLE_STATUS_SUCCESS)
    {
      LOG_INFO_APP("  Fail   : aci_gatt_update_char_value - Appearance, result: 0x%02X\n", ret);
    }
    else
    {
      LOG_INFO_APP("  Success: aci_gatt_update_char_value - Appearance\n");
    }
  }
  else
  {
    LOG_ERROR_APP("GAP role cannot be null\n");
  }

  /* Initialize Default PHY */
  ret = hci_le_set_default_phy(CFG_PHY_PREF, CFG_PHY_PREF_TX, CFG_PHY_PREF_RX);
  if (ret != BLE_STATUS_SUCCESS)
  {
    LOG_INFO_APP("  Fail   : hci_le_set_default_phy command, result: 0x%02X\n", ret);
  }
  else
  {
    LOG_INFO_APP("  Success: hci_le_set_default_phy command\n");
  }

  /* Initialize IO capability */
  bleAppContext.BleApplicationContext_legacy.bleSecurityParam.ioCapability = CFG_IO_CAPABILITY;
  ret = aci_gap_set_io_capability(bleAppContext.BleApplicationContext_legacy.bleSecurityParam.ioCapability);
  if (ret != BLE_STATUS_SUCCESS)
  {
    LOG_INFO_APP("  Fail   : aci_gap_set_io_capability command, result: 0x%02X\n", ret);
  }
  else
  {
    LOG_INFO_APP("  Success: aci_gap_set_io_capability command\n");
  }

  /* Initialize authentication */
  bleAppContext.BleApplicationContext_legacy.bleSecurityParam.mitm_mode             = CFG_MITM_PROTECTION;
  bleAppContext.BleApplicationContext_legacy.bleSecurityParam.encryptionKeySizeMin  = CFG_ENCRYPTION_KEY_SIZE_MIN;
  bleAppContext.BleApplicationContext_legacy.bleSecurityParam.encryptionKeySizeMax  = CFG_ENCRYPTION_KEY_SIZE_MAX;
  bleAppContext.BleApplicationContext_legacy.bleSecurityParam.Use_Fixed_Pin         = CFG_USED_FIXED_PIN;
  bleAppContext.BleApplicationContext_legacy.bleSecurityParam.Fixed_Pin             = CFG_FIXED_PIN;
  bleAppContext.BleApplicationContext_legacy.bleSecurityParam.bonding_mode          = CFG_BONDING_MODE;
  /* USER CODE BEGIN Ble_Hci_Gap_Gatt_Init_1 */

  /* USER CODE END Ble_Hci_Gap_Gatt_Init_1 */

  ret = aci_gap_set_authentication_requirement(bleAppContext.BleApplicationContext_legacy.bleSecurityParam.bonding_mode,
                                               bleAppContext.BleApplicationContext_legacy.bleSecurityParam.mitm_mode,
                                               CFG_SC_SUPPORT,
                                               CFG_KEYPRESS_NOTIFICATION_SUPPORT,
                                               bleAppContext.BleApplicationContext_legacy.bleSecurityParam.encryptionKeySizeMin,
                                               bleAppContext.BleApplicationContext_legacy.bleSecurityParam.encryptionKeySizeMax,
                                               bleAppContext.BleApplicationContext_legacy.bleSecurityParam.Use_Fixed_Pin,
                                               bleAppContext.BleApplicationContext_legacy.bleSecurityParam.Fixed_Pin,
                                               CFG_BD_ADDRESS_DEVICE);
  if (ret != BLE_STATUS_SUCCESS)
  {
    LOG_INFO_APP("  Fail   : aci_gap_set_authentication_requirement command, result: 0x%02X\n", ret);
  }
  else
  {
    LOG_INFO_APP("  Success: aci_gap_set_authentication_requirement command\n");
  }

  /* Initialize whitelist */
  if (bleAppContext.BleApplicationContext_legacy.bleSecurityParam.bonding_mode)
  {
    ret = aci_gap_configure_whitelist();
    if (ret != BLE_STATUS_SUCCESS)
    {
      LOG_INFO_APP("  Fail   : aci_gap_configure_whitelist command, result: 0x%02X\n", ret);
    }
    else
    {
      LOG_INFO_APP("  Success: aci_gap_configure_whitelist command\n");
    }
  }

  /* USER CODE BEGIN Ble_Hci_Gap_Gatt_Init_2 */

  /* USER CODE END Ble_Hci_Gap_Gatt_Init_2 */

  LOG_INFO_APP("==>> End Ble_Hci_Gap_Gatt_Init function\n");

  return;
}

static void Ble_UserEvtRx( void)
{
  SVCCTL_UserEvtFlowStatus_t svctl_return_status;
  BleEvtPacket_t *phcievt = NULL;

  LST_remove_head ( &BleAsynchEventQueue, (tListNode **)&phcievt );

  svctl_return_status = SVCCTL_UserEvtRx((void *)&(phcievt->evtserial));

  if (svctl_return_status != SVCCTL_UserEvtFlowDisable)
  {
    AMM_Free((uint32_t *)phcievt);
  }
  else
  {
    LST_insert_head ( &BleAsynchEventQueue, (tListNode *)phcievt );
  }

  if ((LST_is_empty(&BleAsynchEventQueue) == FALSE) && (svctl_return_status != SVCCTL_UserEvtFlowDisable) )
  {
    UTIL_SEQ_SetTask(1U << CFG_TASK_HCI_ASYNCH_EVT_ID, CFG_SEQ_PRIO_0);
  }

  /* Trigger BLE Host stack to process */
  UTIL_SEQ_SetTask(1U << CFG_TASK_BLE_HOST, CFG_SEQ_PRIO_0);

}

static const uint8_t* BleGenerateBdAddress(void)
{
  OTP_Data_s *p_otp_addr = NULL;
  const uint8_t *p_bd_addr;
  uint32_t udn;
  uint32_t company_id;
  uint32_t device_id;
  uint8_t a_BdAddrDefault[BD_ADDR_SIZE] ={0x65, 0x43, 0x21, 0x1E, 0x08, 0x00};
  uint8_t a_BDAddrNull[BD_ADDR_SIZE];
  memset(&a_BDAddrNull[0], 0x00, sizeof(a_BDAddrNull));

  a_BdAddr[0] = (uint8_t)(CFG_BD_ADDRESS & 0x0000000000FF);
  a_BdAddr[1] = (uint8_t)((CFG_BD_ADDRESS & 0x00000000FF00) >> 8);
  a_BdAddr[2] = (uint8_t)((CFG_BD_ADDRESS & 0x000000FF0000) >> 16);
  a_BdAddr[3] = (uint8_t)((CFG_BD_ADDRESS & 0x0000FF000000) >> 24);
  a_BdAddr[4] = (uint8_t)((CFG_BD_ADDRESS & 0x00FF00000000) >> 32);
  a_BdAddr[5] = (uint8_t)((CFG_BD_ADDRESS & 0xFF0000000000) >> 40);

  if(memcmp(&a_BdAddr[0], &a_BDAddrNull[0], BD_ADDR_SIZE) != 0)
  {
    p_bd_addr = (const uint8_t *)a_BdAddr;
  }
  else
  {
    udn = LL_FLASH_GetUDN();

    /* USER CODE BEGIN BleGenerateBdAddress */

    /* USER CODE END BleGenerateBdAddress */

    if (udn != 0xFFFFFFFF)
    {
      company_id = LL_FLASH_GetSTCompanyID();
      device_id = LL_FLASH_GetDeviceID();

    /**
     * Public Address with the ST company ID
     * bit[47:24] : 24bits (OUI) equal to the company ID
     * bit[23:16] : Device ID.
     * bit[15:0] : The last 16bits from the UDN
     * Note: In order to use the Public Address in a final product, a dedicated
     * 24bits company ID (OUI) shall be bought.
     */
      a_BdAddr[0] = (uint8_t)(udn & 0x000000FF);
      a_BdAddr[1] = (uint8_t)((udn & 0x0000FF00) >> 8);
      a_BdAddr[2] = (uint8_t)device_id;
      a_BdAddr[3] = (uint8_t)(company_id & 0x000000FF);
      a_BdAddr[4] = (uint8_t)((company_id & 0x0000FF00) >> 8);
      a_BdAddr[5] = (uint8_t)((company_id & 0x00FF0000) >> 16);
      p_bd_addr = (const uint8_t *)a_BdAddr;
    }
    else
    {
      if (OTP_Read(0, &p_otp_addr) == HAL_OK)
      {
        a_BdAddr[0] = p_otp_addr->bd_address[0];
        a_BdAddr[1] = p_otp_addr->bd_address[1];
        a_BdAddr[2] = p_otp_addr->bd_address[2];
        a_BdAddr[3] = p_otp_addr->bd_address[3];
        a_BdAddr[4] = p_otp_addr->bd_address[4];
        a_BdAddr[5] = p_otp_addr->bd_address[5];
        p_bd_addr = (const uint8_t *)a_BdAddr;
      }
      else
      {
        memcpy(&a_BdAddr[0], a_BdAddrDefault,BD_ADDR_SIZE);
        p_bd_addr = (const uint8_t *)a_BdAddr;
      }
    }
  }

  return p_bd_addr;
}

static const uint8_t* BleGenerateIRValue(void)
{
  uint32_t uid_word0;
  uint32_t uid_word1;
  const uint8_t *p_ir_value;
  uint8_t a_BLE_CfgIrValueNull[16];
  uint8_t a_cfg_ir_value[16] = CFG_BLE_IR;
  uint8_t a_BLE_CfgIrValueDefault[16] =
  {
    0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0
  };

  /* USER CODE BEGIN BleGenerateIRValue_1 */

  /* USER CODE END BleGenerateIRValue_1 */

  memset(&a_BLE_CfgIrValueNull[0], 0x00, sizeof(a_BLE_CfgIrValueNull));

  memcpy(&a_BLE_CfgIrValue[0], a_cfg_ir_value,16);

  if(memcmp(&a_BLE_CfgIrValue[0], &a_BLE_CfgIrValueNull[0], 16) != 0)
  {
    p_ir_value = (const uint8_t *)a_BLE_CfgIrValue;
  }
  else
  {
    uid_word0 = LL_GetUID_Word0();
    uid_word1 = LL_GetUID_Word1();
    /* USER CODE BEGIN BleGenerateIRValue_2 */

    /* USER CODE END BleGenerateIRValue_2 */

    if ((uid_word0 != 0xFFFFFFFF) && (uid_word1 != 0xFFFFFFFF))
    {

    /**
     * Identity root key is built from bits of the UDN.
     */
      a_BLE_CfgIrValue[0] = a_BLE_CfgIrValue[8] = (uint8_t)(uid_word0 & 0x000000FF);
      a_BLE_CfgIrValue[1] = a_BLE_CfgIrValue[9] = (uint8_t)((uid_word0 & 0x0000FF00) >> 8);
      a_BLE_CfgIrValue[2] = a_BLE_CfgIrValue[10] = (uint8_t)((uid_word0 & 0x00FF0000) >> 16);
      a_BLE_CfgIrValue[3] = a_BLE_CfgIrValue[11] = (uint8_t)((uid_word0 & 0xFF000000) >> 24);
      a_BLE_CfgIrValue[4] = a_BLE_CfgIrValue[12] = (uint8_t)(uid_word1 & 0x000000FF);
      a_BLE_CfgIrValue[5] = a_BLE_CfgIrValue[13] = (uint8_t)((uid_word1 & 0x0000FF00) >> 8);
      a_BLE_CfgIrValue[6] = a_BLE_CfgIrValue[14] = (uint8_t)((uid_word1 & 0x00FF0000) >> 16);
      a_BLE_CfgIrValue[7] = a_BLE_CfgIrValue[15] = (uint8_t)((uid_word1 & 0xFF000000) >> 24);
      p_ir_value = (const uint8_t *)a_BLE_CfgIrValue;
    }
    else
    {
      memcpy(&a_BLE_CfgIrValue[0], a_BLE_CfgIrValueDefault,16);
      p_ir_value = (const uint8_t *)a_BLE_CfgIrValue;
    }
  }

  /* USER CODE BEGIN BleGenerateIRValue_3 */

  /* USER CODE END BleGenerateIRValue_3 */
  return p_ir_value;
}

static const uint8_t* BleGenerateERValue(void)
{
  const uint8_t *p_er_value;
  uint32_t uid_word1;
  uint32_t uid_word2;
  uint8_t a_BLE_CfgErValueNull[16];
  uint8_t a_cfg_er_value[16] = CFG_BLE_ER;
  uint8_t a_BLE_CfgErValueDefault[16] =
  {
    0xFE, 0xDC, 0xBA, 0x09, 0x87, 0x65, 0x43, 0x21, 0xFE, 0xDC, 0xBA, 0x09, 0x87, 0x65, 0x43, 0x21
  };

  /* USER CODE BEGIN BleGenerateERValue_1 */

  /* USER CODE END BleGenerateERValue_1 */

  memset(&a_BLE_CfgErValueNull[0], 0x00, sizeof(a_BLE_CfgErValueNull));

  memcpy(&a_BLE_CfgErValue[0], a_cfg_er_value,16);

  if(memcmp(&a_BLE_CfgErValue[0], &a_BLE_CfgErValueNull[0], 16) != 0)
  {
    p_er_value = (const uint8_t *)a_BLE_CfgErValue;
  }
  else
  {
    uid_word1 = LL_GetUID_Word1();
    uid_word2 = LL_GetUID_Word2();

    /* USER CODE BEGIN BleGenerateERValue_2 */

    /* USER CODE END BleGenerateERValue_2 */

   if ((uid_word1 != 0xFFFFFFFF) && (uid_word2 != 0xFFFFFFFF))
    {

    /**
     * Encryption root key is built from bits of the UDN.
     */
      a_BLE_CfgErValue[0] = a_BLE_CfgErValue[8] = (uint8_t)(uid_word2 & 0x000000FF);
      a_BLE_CfgErValue[1] = a_BLE_CfgErValue[9] = (uint8_t)((uid_word2 & 0x0000FF00) >> 8);
      a_BLE_CfgErValue[2] = a_BLE_CfgErValue[10] = (uint8_t)((uid_word2 & 0x00FF0000) >> 16);
      a_BLE_CfgErValue[3] = a_BLE_CfgErValue[11] = (uint8_t)((uid_word2 & 0xFF000000) >> 24);
      a_BLE_CfgErValue[4] = a_BLE_CfgErValue[12] = (uint8_t)~(uid_word1 & 0x000000FF);
      a_BLE_CfgErValue[5] = a_BLE_CfgErValue[13] = (uint8_t)~((uid_word1 & 0x0000FF00) >> 8);
      a_BLE_CfgErValue[6] = a_BLE_CfgErValue[14] = (uint8_t)~((uid_word1 & 0x00FF0000) >> 16);
      a_BLE_CfgErValue[7] = a_BLE_CfgErValue[15] = (uint8_t)~((uid_word1 & 0xFF000000) >> 24);
      p_er_value = (const uint8_t *)a_BLE_CfgErValue;
    }
    else
    {
      memcpy(&a_BLE_CfgErValue[0], a_BLE_CfgErValueDefault,16);
      p_er_value = (const uint8_t *)a_BLE_CfgErValue;
    }
  }

  /* USER CODE BEGIN BleGenerateERValue_3 */

  /* USER CODE END BleGenerateERValue_3 */
  return p_er_value;
}

static void BleStack_Process_BG(void)
{
  if (BleStack_Process( ) == 0x0)
  {
    BleStackCB_Process( );
  }
}

static void gap_cmd_resp_release(void)
{
  UTIL_SEQ_SetEvt(1U << CFG_IDLEEVT_PROC_GAP_COMPLETE);
  return;
}

static void gap_cmd_resp_wait(void)
{
  UTIL_SEQ_WaitEvt(1U << CFG_IDLEEVT_PROC_GAP_COMPLETE);
  return;
}

/**
  * @brief  Notify the LL to resume the flow process
  * @param  None
  * @retval None
  */
static void BLE_ResumeFlowProcessCallback(void)
{
  /* Receive any events from the LL. */
  change_state_options_t notify_options;

  notify_options.combined_value = 0x0F;

  ll_intf_chng_evnt_hndlr_state( notify_options );
}

/* USER CODE BEGIN FD_LOCAL_FUNCTION */
static void Bridge_PutU16LE(uint8_t *p, uint16_t value)
{
  p[0] = (uint8_t)(value & 0x00FFU);
  p[1] = (uint8_t)(value >> 8);
}

static uint16_t Bridge_GetU16LE(const uint8_t *p)
{
  return (uint16_t)(((uint16_t)p[1] << 8U) | p[0]);
}

static void Bridge_StatePayloadInit(void)
{
  memset(a_StatePayload, 0, sizeof(a_StatePayload));
  a_StatePayload[APP_BLE_BRIDGE_OFF_PROTO] = APP_BLE_BRIDGE_PROTO_VER;
  Bridge_PutU16LE(&a_StatePayload[APP_BLE_BRIDGE_OFF_DEVICE_ID],
                  APP_BLE_BRIDGE_LOCAL_DEVICE_ID);
  a_StatePayload[APP_BLE_BRIDGE_OFF_POS_QUALITY] = 0U;
  a_StatePayload[APP_BLE_BRIDGE_OFF_GPS_AGE_Q] = 0xFFU;
  a_StatePayload[APP_BLE_BRIDGE_OFF_RESERVED] = 0U;
}

static void Bridge_StatePayloadTouch(void)
{
  uint32_t now = HAL_GetTick();

  if ((bridgeUartStateActive != 0U) &&
      ((now - bridgeUartStateLastRxTick) <= APP_BLE_BRIDGE_UART_STATE_TIMEOUT_MS))
  {
    a_StatePayload[APP_BLE_BRIDGE_OFF_PROTO] = APP_BLE_BRIDGE_PROTO_VER;
    return;
  }

  bridgeUartStateActive = 0U;
  if (bridgeUartStateSeen != 0U)
  {
    a_StatePayload[APP_BLE_BRIDGE_OFF_STATUS_FLAGS] &=
      (uint8_t)~(APP_BLE_BRIDGE_STATUS_MOVING | APP_BLE_BRIDGE_STATUS_GPS_VALID);
    a_StatePayload[APP_BLE_BRIDGE_OFF_STATUS_FLAGS] |= APP_BLE_BRIDGE_STATUS_LOW_CONFIDENCE;
    a_StatePayload[APP_BLE_BRIDGE_OFF_SPEED_Q] = 0U;
    Bridge_PutU16LE(&a_StatePayload[APP_BLE_BRIDGE_OFF_HEADING_Q], 0U);
    Bridge_PutU16LE(&a_StatePayload[APP_BLE_BRIDGE_OFF_LAT_LOW16], 0U);
    Bridge_PutU16LE(&a_StatePayload[APP_BLE_BRIDGE_OFF_LON_LOW16], 0U);
    a_StatePayload[APP_BLE_BRIDGE_OFF_POS_QUALITY] = 0U;
    a_StatePayload[APP_BLE_BRIDGE_OFF_GPS_AGE_Q] = 0xFFU;
    a_StatePayload[APP_BLE_BRIDGE_OFF_MSG_COUNTER] = msgCounter++;
  }
  else
  {
    Bridge_BuildStatePayloadFromSensors();
    a_StatePayload[APP_BLE_BRIDGE_OFF_MSG_COUNTER] = msgCounter++;
  }
  a_StatePayload[APP_BLE_BRIDGE_OFF_PROTO] = APP_BLE_BRIDGE_PROTO_VER;
}

static uint16_t Bridge_Coord1e7ToLow16(int32_t coord_1e7)
{
  int32_t coord_1e5;

  if (coord_1e7 >= 0)
  {
    coord_1e5 = (coord_1e7 + 50) / 100;
  }
  else
  {
    coord_1e5 = (coord_1e7 - 50) / 100;
  }

  return (uint16_t)((uint32_t)coord_1e5 & 0xFFFFU);
}

static uint8_t Bridge_EncodeSpeedQ(uint16_t speed_cms)
{
  uint32_t q = ((uint32_t)speed_cms + 12U) / 25U;

  return (q > 255U) ? 255U : (uint8_t)q;
}

static uint16_t Bridge_EncodeHeadingQ(uint16_t heading_cdeg)
{
  uint32_t q = ((uint32_t)heading_cdeg * 65536U + 18000U) / 36000U;

  return (uint16_t)(q & 0xFFFFU);
}

static int32_t Bridge_NormalizeHeadingCdeg(int32_t heading_cdeg)
{
  while (heading_cdeg < 0)
  {
    heading_cdeg += 36000;
  }
  while (heading_cdeg >= 36000)
  {
    heading_cdeg -= 36000;
  }

  return heading_cdeg;
}

static int32_t Bridge_HeadingDeltaCdeg(int32_t target_cdeg, int32_t reference_cdeg)
{
  int32_t delta = Bridge_NormalizeHeadingCdeg(target_cdeg - reference_cdeg);

  if (delta > 18000)
  {
    delta -= 36000;
  }

  return delta;
}

static int32_t Bridge_ImuYawToHeadingCdeg(int16_t yaw_raw)
{
  int32_t heading_cdeg;

  heading_cdeg = ((int32_t)yaw_raw * 18000) / 32768;
#if (APP_BLE_BRIDGE_IMU_YAW_TO_HEADING_SIGN < 0)
  heading_cdeg = -heading_cdeg;
#endif

  return Bridge_NormalizeHeadingCdeg(heading_cdeg);
}

static uint16_t Bridge_EncodeImuYawHeadingQ(int16_t yaw_raw)
{
  int32_t heading_cdeg = Bridge_ImuYawToHeadingCdeg(yaw_raw);

  heading_cdeg = Bridge_NormalizeHeadingCdeg(heading_cdeg + bridgeImuHeadingOffsetCdeg);

  return Bridge_EncodeHeadingQ((uint16_t)heading_cdeg);
}

static int8_t Bridge_EncodeYawRateQ(int16_t gyro_z_raw)
{
  int32_t q;

  if (gyro_z_raw >= 0)
  {
    q = ((int32_t)gyro_z_raw + 16) / 32;
  }
  else
  {
    q = -(((int32_t)(-gyro_z_raw) + 16) / 32);
  }

  if (q < -128)
  {
    q = -128;
  }
  else if (q > 127)
  {
    q = 127;
  }

  return (int8_t)q;
}

static uint8_t Bridge_EncodeGpsAgeQ(uint32_t now, uint32_t gps_tick, uint8_t gps_valid)
{
  uint32_t age_q;

  if (gps_valid == 0U)
  {
    return 0xFFU;
  }

  age_q = (now - gps_tick + (APP_BLE_BRIDGE_GPS_AGE_UNIT_MS / 2U)) /
          APP_BLE_BRIDGE_GPS_AGE_UNIT_MS;

  return (age_q > 255U) ? 255U : (uint8_t)age_q;
}

static void Bridge_UpdateHeadingCalibration(const GP21_GnssState_t *gps,
                                            const IMU_SensorState_t *imu,
                                            uint32_t now)
{
  int32_t candidate_cdeg;
  int32_t delta_cdeg;
  int8_t yaw_rate_q;

  if ((gps == NULL) || (imu == NULL) ||
      (gps->last_update_ms == bridgeHeadingCalGpsTick))
  {
    return;
  }
  bridgeHeadingCalGpsTick = gps->last_update_ms;

  if ((gps->valid == 0U) ||
      ((now - gps->last_update_ms) > APP_BLE_BRIDGE_GPS_VALID_MAX_AGE_MS) ||
      (gps->speed_cms < APP_BLE_BRIDGE_HEADING_CAL_MIN_SPEED_CMS) ||
      (imu->attitude_valid == 0U) ||
      ((now - imu->attitude_update_ms) > APP_BLE_BRIDGE_IMU_VALID_MAX_AGE_MS) ||
      (imu->six_axis_valid == 0U) ||
      ((now - imu->six_axis_update_ms) > APP_BLE_BRIDGE_IMU_VALID_MAX_AGE_MS))
  {
    bridgeHeadingCalCandidateCount = 0U;
    return;
  }

  yaw_rate_q = Bridge_EncodeYawRateQ(imu->gz);
  if ((yaw_rate_q >= APP_BLE_BRIDGE_TURNING_MIN_YAW_RATE_Q) ||
      (yaw_rate_q <= -APP_BLE_BRIDGE_TURNING_MIN_YAW_RATE_Q))
  {
    bridgeHeadingCalCandidateCount = 0U;
    return;
  }

  /* Stable GPS course refines the 9-axis sensor's mounting and magnetic-north offset. */
  candidate_cdeg = Bridge_NormalizeHeadingCdeg((int32_t)gps->heading_cdeg -
                                                Bridge_ImuYawToHeadingCdeg(imu->yaw));
  if (bridgeHeadingCalCandidateCount == 0U)
  {
    bridgeHeadingCalCandidateCdeg = candidate_cdeg;
    bridgeHeadingCalCandidateCount = 1U;
    return;
  }

  delta_cdeg = Bridge_HeadingDeltaCdeg(candidate_cdeg, bridgeHeadingCalCandidateCdeg);
  if ((delta_cdeg > APP_BLE_BRIDGE_HEADING_CAL_MAX_STEP_CDEG) ||
      (delta_cdeg < -APP_BLE_BRIDGE_HEADING_CAL_MAX_STEP_CDEG))
  {
    bridgeHeadingCalCandidateCdeg = candidate_cdeg;
    bridgeHeadingCalCandidateCount = 1U;
    return;
  }

  bridgeHeadingCalCandidateCdeg =
    Bridge_NormalizeHeadingCdeg(bridgeHeadingCalCandidateCdeg +
                                (delta_cdeg / (int32_t)(bridgeHeadingCalCandidateCount + 1U)));
  if (bridgeHeadingCalCandidateCount < APP_BLE_BRIDGE_HEADING_CAL_CONFIRM_COUNT)
  {
    bridgeHeadingCalCandidateCount++;
  }

  if (bridgeHeadingCalCandidateCount >= APP_BLE_BRIDGE_HEADING_CAL_CONFIRM_COUNT)
  {
    if (bridgeHeadingCalValid == 0U)
    {
      bridgeImuHeadingOffsetCdeg = bridgeHeadingCalCandidateCdeg;
    }
    else
    {
      delta_cdeg = Bridge_HeadingDeltaCdeg(bridgeHeadingCalCandidateCdeg,
                                           bridgeImuHeadingOffsetCdeg);
      bridgeImuHeadingOffsetCdeg =
        Bridge_NormalizeHeadingCdeg(bridgeImuHeadingOffsetCdeg + (delta_cdeg / 4));
    }
    bridgeHeadingCalValid = 1U;
  }
}

static void Bridge_BuildStatePayloadFromSensors(void)
{
  GP21_GnssState_t gps;
  IMU_SensorState_t imu;
  uint32_t now = HAL_GetTick();
  uint8_t status = 0U;
  uint8_t gpsFresh;
  uint8_t imuSixAxisFresh;
  uint8_t imuHeadingFresh;
  uint8_t speed_q;
  uint16_t heading_q = 0U;
  int8_t yaw_rate_q = 0;

  GP21_Gnss_GetState(&gps);
  IMU_Sensor_GetState(&imu);

  gpsFresh = ((gps.valid != 0U) &&
              ((now - gps.last_update_ms) <= APP_BLE_BRIDGE_GPS_VALID_MAX_AGE_MS)) ? 1U : 0U;
  imuSixAxisFresh = ((imu.six_axis_valid != 0U) &&
                     ((now - imu.six_axis_update_ms) <= APP_BLE_BRIDGE_IMU_VALID_MAX_AGE_MS)) ? 1U : 0U;
  imuHeadingFresh = Bridge_IsImuHeadingFresh(&imu, now);

  speed_q = gpsFresh ? Bridge_EncodeSpeedQ(gps.speed_cms) : 0U;
  if (imuSixAxisFresh != 0U)
  {
    yaw_rate_q = Bridge_EncodeYawRateQ(imu.gz);
  }
  Bridge_UpdateHeadingCalibration(&gps, &imu, now);
  if ((gpsFresh != 0U) &&
      (gps.speed_cms >= APP_BLE_BRIDGE_GPS_COURSE_MIN_SPEED_CMS))
  {
    heading_q = Bridge_EncodeHeadingQ(gps.heading_cdeg);
  }
  else if (imuHeadingFresh != 0U)
  {
    heading_q = Bridge_EncodeImuYawHeadingQ(imu.yaw);
  }

  if (speed_q >= APP_BLE_BRIDGE_MOVING_MIN_SPEED_Q)
  {
    status |= APP_BLE_BRIDGE_STATUS_MOVING;
  }
  if ((yaw_rate_q >= APP_BLE_BRIDGE_TURNING_MIN_YAW_RATE_Q) ||
      (yaw_rate_q <= -APP_BLE_BRIDGE_TURNING_MIN_YAW_RATE_Q))
  {
    status |= APP_BLE_BRIDGE_STATUS_TURNING;
  }
  if ((imuSixAxisFresh != 0U) && (imu.ay < -1500))
  {
    status |= APP_BLE_BRIDGE_STATUS_BRAKING;
  }
  if (gpsFresh != 0U)
  {
    status |= APP_BLE_BRIDGE_STATUS_GPS_VALID;
  }
  if ((imuSixAxisFresh != 0U) || (imuHeadingFresh != 0U))
  {
    status |= APP_BLE_BRIDGE_STATUS_IMU_VALID;
  }
  if ((gpsFresh == 0U) || (gps.pos_quality < 2U))
  {
    status |= APP_BLE_BRIDGE_STATUS_LOW_CONFIDENCE;
  }

  a_StatePayload[APP_BLE_BRIDGE_OFF_STATUS_FLAGS] = status;
  a_StatePayload[APP_BLE_BRIDGE_OFF_SPEED_Q] = speed_q;
  Bridge_PutU16LE(&a_StatePayload[APP_BLE_BRIDGE_OFF_HEADING_Q], heading_q);
  a_StatePayload[APP_BLE_BRIDGE_OFF_YAW_RATE_Q] = (uint8_t)yaw_rate_q;
  Bridge_PutU16LE(&a_StatePayload[APP_BLE_BRIDGE_OFF_LAT_LOW16],
                  gpsFresh ? Bridge_Coord1e7ToLow16(gps.latitude_1e7) : 0U);
  Bridge_PutU16LE(&a_StatePayload[APP_BLE_BRIDGE_OFF_LON_LOW16],
                  gpsFresh ? Bridge_Coord1e7ToLow16(gps.longitude_1e7) : 0U);
  a_StatePayload[APP_BLE_BRIDGE_OFF_POS_QUALITY] = gpsFresh ? gps.pos_quality : 0U;
  a_StatePayload[APP_BLE_BRIDGE_OFF_GPS_AGE_Q] =
    Bridge_EncodeGpsAgeQ(now, gps.last_update_ms, gpsFresh);
  a_StatePayload[APP_BLE_BRIDGE_OFF_RESERVED] = 0U;
}

static void Bridge_FillMfgAd(uint8_t *pData, uint8_t *pLen)
{
  uint8_t i = 0U;

  pData[i++] = (uint8_t)(1U + APP_BLE_BRIDGE_MFG_PAYLOAD_LEN);
  pData[i++] = AD_TYPE_MANUFACTURER_SPECIFIC_DATA;
  pData[i++] = APP_BLE_BRIDGE_COMPANY_ID_L;
  pData[i++] = APP_BLE_BRIDGE_COMPANY_ID_H;
  pData[i++] = APP_BLE_BRIDGE_MAGIC_L;
  pData[i++] = APP_BLE_BRIDGE_MAGIC_H;
  memcpy(&pData[i], a_StatePayload, APP_BLE_BRIDGE_STATE_PAYLOAD_LEN);
  i += APP_BLE_BRIDGE_STATE_PAYLOAD_LEN;
  *pLen = i;
}

static void Bridge_FillAdvertisingData(void)
{
  uint8_t i = 0U;
  uint8_t mfgLen = 0U;

  a_AdvData[i++] = 0x02U;
  a_AdvData[i++] = AD_TYPE_FLAGS;
  a_AdvData[i++] = FLAG_BIT_LE_GENERAL_DISCOVERABLE_MODE | FLAG_BIT_BR_EDR_NOT_SUPPORTED;
  a_AdvData[i++] = 0x03U;
  a_AdvData[i++] = AD_TYPE_SHORTENED_LOCAL_NAME;
  a_AdvData[i++] = 'M';
  a_AdvData[i++] = 'R';
  Bridge_FillMfgAd(&a_AdvData[i], &mfgLen);
  i += mfgLen;
  a_AdvDataLen = i;

  i = 0U;
  a_ScanRspData[i++] = 0x03U;
  a_ScanRspData[i++] = AD_TYPE_SHORTENED_LOCAL_NAME;
  a_ScanRspData[i++] = 'M';
  a_ScanRspData[i++] = 'R';
  Bridge_FillMfgAd(&a_ScanRspData[i], &mfgLen);
  i += mfgLen;
  a_ScanRspDataLen = i;
}

static void Bridge_UpdateAdvertisingData(void)
{
  Bridge_StatePayloadTouch();
  Bridge_FillAdvertisingData();
  bridgeStarted = 1U;
}

static void Bridge_StartScan(void)
{
  (void)aci_gap_start_observation_proc(SCAN_INT_MS(100),
                                       SCAN_WIN_MS(50),
                                       HCI_SCAN_TYPE_ACTIVE,
                                       CFG_BD_ADDRESS_TYPE,
                                       0x00U,
                                       HCI_SCAN_FILTER_NO);
}

static uint8_t Bridge_UartChecksum(uint8_t type, uint8_t len, const uint8_t *payload)
{
  uint8_t chk = type ^ len;

  for (uint8_t i = 0U; i < len; i++)
  {
    chk ^= payload[i];
  }

  return chk;
}

static void Bridge_UartSendFrame(uint8_t type, const uint8_t *payload, uint8_t len)
{
  uint8_t frame[APP_BLE_BRIDGE_UART_MAX_PAYLOAD_LEN + 5U];
  uint8_t i = 0U;

  if ((len > APP_BLE_BRIDGE_UART_MAX_PAYLOAD_LEN) ||
      ((len > 0U) && (payload == NULL)))
  {
    return;
  }

  frame[i++] = APP_BLE_BRIDGE_UART_FRAME_H0;
  frame[i++] = APP_BLE_BRIDGE_UART_FRAME_H1;
  frame[i++] = type;
  frame[i++] = len;
  if (len > 0U)
  {
    memcpy(&frame[i], payload, len);
    i += len;
  }
  frame[i++] = Bridge_UartChecksum(type, len, payload);

  (void)HAL_UART_Transmit(&huart1, frame, i, 20U);
}

static void Bridge_UartProcessRx(void)
{
  uint16_t offset = 0U;

  while ((uartRxLen - offset) >= 5U)
  {
    uint8_t type;
    uint8_t len;
    uint16_t frameLen;

    if ((a_UartRxBuf[offset] != APP_BLE_BRIDGE_UART_FRAME_H0) ||
        (a_UartRxBuf[offset + 1U] != APP_BLE_BRIDGE_UART_FRAME_H1))
    {
      offset++;
      continue;
    }

    type = a_UartRxBuf[offset + 2U];
    len = a_UartRxBuf[offset + 3U];
    if (len > APP_BLE_BRIDGE_UART_MAX_PAYLOAD_LEN)
    {
      offset++;
      continue;
    }

    frameLen = (uint16_t)len + 5U;
    if ((uartRxLen - offset) < frameLen)
    {
      break;
    }

    if (Bridge_UartChecksum(type, len, &a_UartRxBuf[offset + 4U]) ==
        a_UartRxBuf[offset + 4U + len])
    {
      if ((type == APP_BLE_BRIDGE_UART_TYPE_TX_STATE) &&
          (len == APP_BLE_BRIDGE_STATE_PAYLOAD_LEN) &&
          (a_UartRxBuf[offset + 4U + APP_BLE_BRIDGE_OFF_PROTO] ==
           APP_BLE_BRIDGE_PROTO_VER) &&
          (Bridge_GetU16LE(&a_UartRxBuf[offset + 4U + APP_BLE_BRIDGE_OFF_DEVICE_ID]) ==
           APP_BLE_BRIDGE_EXPECTED_PEER_ID))
      {
        memcpy(a_StatePayload, &a_UartRxBuf[offset + 4U], APP_BLE_BRIDGE_STATE_PAYLOAD_LEN);
        a_StatePayload[APP_BLE_BRIDGE_OFF_PROTO] = APP_BLE_BRIDGE_PROTO_VER;
        a_StatePayload[APP_BLE_BRIDGE_OFF_MSG_COUNTER] = msgCounter++;
        bridgeUartStateLastRxTick = HAL_GetTick();
        bridgeUartStateActive = 1U;
        bridgeUartStateSeen = 1U;
#if (APP_BLE_BRIDGE_RADIO_ENABLE != 0U)
        APP_BLE_Procedure_Gap_Peripheral(PROC_GAP_PERIPH_ADVERTISE_DATA_UPDATE);
#if (APP_BLE_BRIDGE_BINARY_DEBUG_FRAME_ENABLE != 0U)
        Bridge_UartSendFrame(APP_BLE_BRIDGE_UART_TYPE_LOCAL_STATE,
                             a_StatePayload,
                             APP_BLE_BRIDGE_STATE_PAYLOAD_LEN);
#endif
#endif
      }
      offset += frameLen;
    }
    else
    {
      offset++;
    }
  }

  if (offset > 0U)
  {
    uartRxLen -= offset;
    if (uartRxLen > 0U)
    {
      memmove(a_UartRxBuf, &a_UartRxBuf[offset], uartRxLen);
    }
  }
}

static void Bridge_HandleRawAdvertisingReports(const uint8_t *pData)
{
  uint8_t payload[APP_BLE_BRIDGE_STATE_PAYLOAD_LEN];
  uint8_t numReports;

  if (pData == NULL)
  {
    return;
  }

  numReports = pData[0];
  pData++;

  for (uint8_t i = 0U; i < numReports; i++)
  {
    Advertising_Report_t report;

    report.Event_Type = pData[0];
    report.Address_Type = pData[1];
    memcpy(report.Address, &pData[2], sizeof(report.Address));
    report.Length_Data = pData[8];
    report.Data = &pData[9];
    report.RSSI = pData[9U + report.Length_Data];

    if (Bridge_FilterAdvertisingReport(&report, payload) != 0U)
    {
      Bridge_HandlePeerPayload(payload, report.RSSI, 1U);
#if (APP_BLE_BRIDGE_BINARY_DEBUG_FRAME_ENABLE != 0U)
      Bridge_UartSendFrame(APP_BLE_BRIDGE_UART_TYPE_SCAN_STATE,
                           payload,
                           APP_BLE_BRIDGE_STATE_PAYLOAD_LEN);
#endif
    }

    pData += (10U + report.Length_Data);
  }
}

static uint8_t Bridge_FindAdType(const uint8_t *adData,
                                 uint8_t adLen,
                                 uint8_t adType,
                                 const uint8_t **ppData,
                                 uint8_t *pDataLen)
{
  uint8_t offset = 0U;

  if ((adData == NULL) || (ppData == NULL) || (pDataLen == NULL))
  {
    return 0U;
  }

  *ppData = NULL;
  *pDataLen = 0U;

  while (offset < adLen)
  {
    uint8_t fieldLen = adData[offset];

    if (fieldLen == 0U)
    {
      break;
    }

    if (((uint16_t)offset + fieldLen) >= adLen)
    {
      break;
    }

    if (adData[offset + 1U] == adType)
    {
      *ppData = &adData[offset + 2U];
      *pDataLen = fieldLen - 1U;
      return 1U;
    }

    offset = (uint8_t)(offset + fieldLen + 1U);
  }

  return 0U;
}

static uint8_t Bridge_FilterAdvertisingReport(const Advertising_Report_t *report, uint8_t *payload)
{
  const uint8_t *mfgData = NULL;
  uint8_t mfgLen = 0U;

  if ((report == NULL) || (payload == NULL))
  {
    return 0U;
  }

  if (Bridge_FindAdType(report->Data,
                        report->Length_Data,
                        AD_TYPE_MANUFACTURER_SPECIFIC_DATA,
                        &mfgData,
                        &mfgLen) == 0U)
  {
    return 0U;
  }

  if (mfgLen < APP_BLE_BRIDGE_MFG_PAYLOAD_LEN)
  {
    return 0U;
  }

  if ((mfgData[0] != APP_BLE_BRIDGE_COMPANY_ID_L) ||
      (mfgData[1] != APP_BLE_BRIDGE_COMPANY_ID_H) ||
      (mfgData[2] != APP_BLE_BRIDGE_MAGIC_L) ||
      (mfgData[3] != APP_BLE_BRIDGE_MAGIC_H))
  {
    return 0U;
  }

  if ((mfgData[4] != APP_BLE_BRIDGE_PROTO_VER) ||
      (Bridge_GetU16LE(&mfgData[4U + APP_BLE_BRIDGE_OFF_DEVICE_ID]) !=
       APP_BLE_BRIDGE_EXPECTED_PEER_ID))
  {
    return 0U;
  }

  memcpy(payload, &mfgData[4], APP_BLE_BRIDGE_STATE_PAYLOAD_LEN);
  filterMatchCount++;
  return 1U;
}

static int16_t Bridge_DiffLow16(uint16_t peer, uint16_t self)
{
  int32_t d = (int32_t)peer - (int32_t)self;

  if (d > 32767)
  {
    d -= 65536;
  }
  else if (d < -32768)
  {
    d += 65536;
  }

  return (int16_t)d;
}

static uint8_t Bridge_IsGpsFresh(const GP21_GnssState_t *gps, uint32_t now)
{
  if (gps == NULL)
  {
    return 0U;
  }

  return ((gps->valid != 0U) &&
          ((now - gps->last_update_ms) <= APP_BLE_BRIDGE_GPS_VALID_MAX_AGE_MS)) ? 1U : 0U;
}

static uint8_t Bridge_IsImuAttitudeFresh(const IMU_SensorState_t *imu, uint32_t now)
{
  if (imu == NULL)
  {
    return 0U;
  }

  return ((imu->attitude_valid != 0U) &&
          ((now - imu->attitude_update_ms) <= APP_BLE_BRIDGE_IMU_VALID_MAX_AGE_MS)) ? 1U : 0U;
}

static uint8_t Bridge_IsImuHeadingFresh(const IMU_SensorState_t *imu, uint32_t now)
{
  return Bridge_IsImuAttitudeFresh(imu, now);
}

/*
 * 把对端相对本机的经纬度差，投影到以本机为中心的本地坐标系。
 *
 * 输入：
 *   dlat_q / dlon_q：对端与本机的纬度/经度差，单位 1e-5 度（约 1.1 米）
 *   latitude_1e7：本机纬度，单位 1e-7 度，用于计算经度方向距离修正
 *   heading_q：本机航向，量化值 0~65535 对应 0~360 度
 *
 * 输出：
 *   x_right_cm：对端在本机右侧为正，左侧为负，单位厘米
 *   y_front_cm：对端在本机前方为正，后方为负，单位厘米
 *
 * 说明：
 *   1) 先把经纬度差换算成东向/北向厘米距离；
 *   2) 再按本机航向做二维旋转，得到"前-右"本地坐标；
 *   3) 三角函数用 16 点查表法（Q15 定点），避免浮点和 libm。
 */
static void Bridge_ProjectToLocalXY(int16_t dlat_q,
                                    int16_t dlon_q,
                                    int32_t latitude_1e7,
                                    uint16_t heading_q,
                                    int32_t *x_right_cm,
                                    int32_t *y_front_cm)
{
  /* 航向 0~360 度均分为 16 个方向的 sin/cos 查表值（Q15） */
  static const int16_t sin_q15[16] =
  {
    0, 12539, 23170, 30274, 32767, 30274, 23170, 12539,
    0, -12539, -23170, -30274, -32767, -30274, -23170, -12539
  };
  static const int16_t cos_q15[16] =
  {
    32767, 30274, 23170, 12539, 0, -12539, -23170, -30274,
    -32767, -30274, -23170, -12539, 0, 12539, 23170, 30274
  };
  uint8_t idx;
  int32_t east_cm;            /* 东向距离（厘米），正东为正 */
  int32_t north_cm;           /* 北向距离（厘米），正北为正 */
  int32_t lon_scale_q15;      /* 纬度圈缩小系数（Q15），高纬度经度方向距离更短 */
  int32_t s;
  int32_t c;

  /* heading_q 归一化到 0~15 的查表索引，+2048 实现四舍五入 */
  idx = (uint8_t)(((uint32_t)heading_q + 2048U) >> 12U) & 0x0FU;

  /* 计算本地经度方向 1e-5 度对应的实际距离比例 */
  lon_scale_q15 = Bridge_LongitudeScaleQ15(latitude_1e7);

  /* 1e-5 度纬度 ≈ 1.11 米 = 111 厘米，乘以差值得到北向厘米距离 */
  north_cm = (int32_t)dlat_q * 111;

  /* 经度方向需乘以纬度圈修正，避免高纬度时距离被算大 */
  east_cm = (int32_t)(((int64_t)dlon_q * 111 * lon_scale_q15) / 32767);

  s = sin_q15[idx];
  c = cos_q15[idx];

  /* 按本机航向旋转：y 为前方分量，x 为右侧分量 */
  *y_front_cm = (int32_t)((((int64_t)east_cm * s) + ((int64_t)north_cm * c)) / 32767);
  *x_right_cm = (int32_t)((((int64_t)east_cm * c) - ((int64_t)north_cm * s)) / 32767);
}

/*
 * 估算两车的相对距离（厘米）。
 * 用"最大值 + 最小值/2"近似勾股定理，避免开方运算。
 * 例：y=300, x=400，实际距离 500cm，估算 = 400 + 300/2 = 550cm，误差约 10%。
 */
static uint32_t Bridge_DistanceApproxCm(int32_t y_front_cm, int32_t x_right_cm)
{
  uint32_t a = (y_front_cm < 0) ? (uint32_t)(-y_front_cm) : (uint32_t)y_front_cm;
  uint32_t b = (x_right_cm < 0) ? (uint32_t)(-x_right_cm) : (uint32_t)x_right_cm;
  uint32_t max_v = (a > b) ? a : b;
  uint32_t min_v = (a > b) ? b : a;

  return max_v + (min_v / 2U);
}

/*
 * V2X 风险判断核心函数。
 * 收到对端状态后，计算相对位置、距离趋势，决定是否触发语音预警。
 */
static void Bridge_HandlePeerPayload(const uint8_t *payload, int8_t rssi, uint8_t rssi_valid)
{
  GP21_GnssState_t self_gps;
  IMU_SensorState_t self_imu;
  uint32_t now = HAL_GetTick();
  uint16_t device_id;
  uint8_t msg_counter;
  uint8_t status_flags;
  uint8_t peer_gps_valid;
  uint8_t self_gps_valid;
  uint8_t self_imu_heading_valid;
  char msg[320] = {0};
  uint16_t len = 0U;

  /* 校验协议版本，版本不对直接丢弃 */
  if ((payload == NULL) || (payload[APP_BLE_BRIDGE_OFF_PROTO] != APP_BLE_BRIDGE_PROTO_VER))
  {
    return;
  }

  /* 解析对端设备 ID、消息序号、状态标志 */
  device_id = Bridge_GetU16LE(&payload[APP_BLE_BRIDGE_OFF_DEVICE_ID]);
  msg_counter = payload[APP_BLE_BRIDGE_OFF_MSG_COUNTER];
  status_flags = payload[APP_BLE_BRIDGE_OFF_STATUS_FLAGS];

  /* 同一个设备的同一帧数据不重复处理（重传或重复接收） */
  if ((peerTrack.valid != 0U) &&
      (peerTrack.device_id == device_id) &&
      (peerTrack.msg_counter == msg_counter))
  {
    return;
  }

  /* 新设备或长时间未见的设备：清空历史滤波和语音状态 */
  if ((peerTrack.valid == 0U) || (peerTrack.device_id != device_id))
  {
    peerTrack.filter_valid = 0U;
    peerTrack.closing_count = 0U;
    peerTrack.range_trend = APP_BLE_BRIDGE_RANGE_TREND_UNKNOWN;
    Bridge_ResetVoiceEvent();
  }

  /* 保存对端最新状态到跟踪结构体 */
  peerTrack.valid = 1U;
  peerTrack.device_id = device_id;
  peerTrack.msg_counter = msg_counter;
  peerTrack.status_flags = status_flags;
  peerTrack.speed_q = payload[APP_BLE_BRIDGE_OFF_SPEED_Q];
  peerTrack.heading_q = Bridge_GetU16LE(&payload[APP_BLE_BRIDGE_OFF_HEADING_Q]);
  peerTrack.yaw_rate_q = (int8_t)payload[APP_BLE_BRIDGE_OFF_YAW_RATE_Q];
  peerTrack.lat_low16 = Bridge_GetU16LE(&payload[APP_BLE_BRIDGE_OFF_LAT_LOW16]);
  peerTrack.lon_low16 = Bridge_GetU16LE(&payload[APP_BLE_BRIDGE_OFF_LON_LOW16]);
  peerTrack.pos_quality = payload[APP_BLE_BRIDGE_OFF_POS_QUALITY];
  peerTrack.gps_age_q = payload[APP_BLE_BRIDGE_OFF_GPS_AGE_Q];
  peerTrack.rssi_valid = rssi_valid;
  peerTrack.rssi = rssi;
  peerTrack.last_rx_ms = now;

#if (APP_BLE_BRIDGE_PEER_DEBUG_ENABLE != 0U)
  Bridge_DebugPayload("DBG: filtered peer", payload, rssi, rssi_valid);
#endif

  /* 判断对端 GPS 数据是否可信：标志位有效、定位质量>0、年龄不过期、坐标非零 */
  peer_gps_valid = (((status_flags & APP_BLE_BRIDGE_STATUS_GPS_VALID) != 0U) &&
                    (peerTrack.pos_quality > 0U) &&
                    (peerTrack.gps_age_q != 0xFFU) &&
#if (APP_BLE_BRIDGE_PEER_GPS_AGE_CHECK_ENABLE != 0U)
                    (peerTrack.gps_age_q <= APP_BLE_BRIDGE_PEER_GPS_MAX_AGE_Q) &&
#endif
                    ((peerTrack.lat_low16 != 0U) || (peerTrack.lon_low16 != 0U))) ? 1U : 0U;

  /* 获取本机 GPS/IMU 状态，并更新 IMU 航向校准 */
  GP21_Gnss_GetState(&self_gps);
  IMU_Sensor_GetState(&self_imu);
  self_gps_valid = Bridge_IsGpsFresh(&self_gps, now);
  self_imu_heading_valid = Bridge_IsImuHeadingFresh(&self_imu, now);
  Bridge_UpdateHeadingCalibration(&self_gps, &self_imu, now);

  /* 距离趋势判断有最小时间间隔，避免同一事件内过度计算 */
  if ((now - peerTrack.last_warn_ms) < APP_BLE_BRIDGE_WARN_INTERVAL_MS)
  {
    return;
  }
  peerTrack.last_warn_ms = now;

  if ((peer_gps_valid == 0U) || (self_gps_valid == 0U))
  {
    len = Bridge_AppendString(msg, len, sizeof(msg), "WARN: peer nearby, ");
    len = Bridge_AppendString(msg, len, sizeof(msg), Bridge_RssiDistanceText(rssi, rssi_valid));
    len = Bridge_AppendString(msg, len, sizeof(msg), "; motion=");
    len = Bridge_AppendString(msg, len, sizeof(msg), Bridge_MotionText(status_flags));
    len = Bridge_AppendString(msg, len, sizeof(msg), "; peer_gps=");
    len = Bridge_AppendUInt(msg, len, sizeof(msg), peer_gps_valid);
    len = Bridge_AppendString(msg, len, sizeof(msg), " self_gps=");
    len = Bridge_AppendUInt(msg, len, sizeof(msg), self_gps_valid);
    if (rssi_valid != 0U)
    {
      len = Bridge_AppendString(msg, len, sizeof(msg), " rssi=");
      len = Bridge_AppendInt(msg, len, sizeof(msg), rssi);
    }
    len = Bridge_AppendString(msg, len, sizeof(msg), " heading_cal=");
    len = Bridge_AppendUInt(msg, len, sizeof(msg), bridgeHeadingCalValid);
    len = Bridge_AppendString(msg, len, sizeof(msg), " self_speed=");
    len = Bridge_AppendUInt(msg, len, sizeof(msg), self_gps.speed_cms);
    len = Bridge_AppendString(msg, len, sizeof(msg), "cm/s");
    len = Bridge_AppendString(msg, len, sizeof(msg), " peer_age_q=");
    len = Bridge_AppendUInt(msg, len, sizeof(msg), peerTrack.gps_age_q);
    len = Bridge_AppendString(msg, len, sizeof(msg), "\r\n");
    Bridge_DebugPrint(msg);
    peerTrack.filter_valid = 0U;
    peerTrack.heading_source = APP_BLE_BRIDGE_HEADING_SRC_NONE;
    if ((peerTrack.range_trend != APP_BLE_BRIDGE_RANGE_TREND_RECEDING) &&
        (Bridge_IsPeerMoving(status_flags) != 0U) &&
        (rssi_valid != 0U) && (rssi >= -75))
    {
      Bridge_UpdateVoiceCandidate(APP_BLE_VOICE_GPS_WEAK,
                                  device_id,
                                  0U,
                                  0,
                                  0,
                                  now);
    }
    else
    {
      Bridge_UpdateVoiceRearm(now);
    }
    return;
  }

  {
    uint16_t self_lat_low16 = Bridge_Coord1e7ToLow16(self_gps.latitude_1e7);
    uint16_t self_lon_low16 = Bridge_Coord1e7ToLow16(self_gps.longitude_1e7);
    int16_t dlat_q = Bridge_DiffLow16(peerTrack.lat_low16, self_lat_low16);
    int16_t dlon_q = Bridge_DiffLow16(peerTrack.lon_low16, self_lon_low16);
    uint16_t local_heading_q;
    int32_t x_right_cm;
    int32_t y_front_cm;
    uint32_t distance_cm;
    uint8_t closing = 0U;
    uint8_t moving_away = 0U;
    uint8_t heading_source = APP_BLE_BRIDGE_HEADING_SRC_NONE;

    /*
     * 选择本机航向来源：
     * 1) GPS 速度足够时优先用 GPS 航向（绝对基准，无累积误差）；
     * 2) GPS 不可靠但 IMU 姿态有效时，用校准后的 IMU yaw；
     * 3) 两者都没有时，只能根据 RSSI 做粗略提示，无法计算相对方向。
     */
    if (self_gps.speed_cms >= APP_BLE_BRIDGE_GPS_COURSE_MIN_SPEED_CMS)
    {
      local_heading_q = Bridge_EncodeHeadingQ(self_gps.heading_cdeg);
      heading_source = APP_BLE_BRIDGE_HEADING_SRC_GPS;
    }
    else if (self_imu_heading_valid != 0U)
    {
      local_heading_q = Bridge_EncodeImuYawHeadingQ(self_imu.yaw);
      heading_source = APP_BLE_BRIDGE_HEADING_SRC_IMU;
    }
    else
    {
      len = Bridge_AppendString(msg, len, sizeof(msg), "WARN: peer nearby, ");
      len = Bridge_AppendString(msg, len, sizeof(msg), Bridge_RssiDistanceText(rssi, rssi_valid));
      len = Bridge_AppendString(msg, len, sizeof(msg), "; motion=");
      len = Bridge_AppendString(msg, len, sizeof(msg), Bridge_MotionText(status_flags));
      len = Bridge_AppendString(msg, len, sizeof(msg), "; heading_src=none");
      len = Bridge_AppendString(msg, len, sizeof(msg), " heading_cal=");
      len = Bridge_AppendUInt(msg, len, sizeof(msg), bridgeHeadingCalValid);
      len = Bridge_AppendString(msg, len, sizeof(msg), " self_speed=");
      len = Bridge_AppendUInt(msg, len, sizeof(msg), self_gps.speed_cms);
      len = Bridge_AppendString(msg, len, sizeof(msg), "cm/s");
      if (rssi_valid != 0U)
      {
        len = Bridge_AppendString(msg, len, sizeof(msg), " rssi=");
        len = Bridge_AppendInt(msg, len, sizeof(msg), rssi);
      }
      len = Bridge_AppendString(msg, len, sizeof(msg), "\r\n");
      Bridge_DebugPrint(msg);
      peerTrack.filter_valid = 0U;
      peerTrack.heading_source = APP_BLE_BRIDGE_HEADING_SRC_NONE;
      if ((peerTrack.range_trend != APP_BLE_BRIDGE_RANGE_TREND_RECEDING) &&
          (Bridge_IsPeerMoving(status_flags) != 0U) &&
          (rssi_valid != 0U) && (rssi >= -75))
      {
        Bridge_UpdateVoiceCandidate(APP_BLE_VOICE_GPS_WEAK,
                                    device_id,
                                    0U,
                                    0,
                                    0,
                                    now);
      }
      else
      {
        Bridge_UpdateVoiceRearm(now);
      }
      return;
    }

    /* 把对端相对位置投影到本地前-右坐标系 */
    Bridge_ProjectToLocalXY(dlat_q,
                            dlon_q,
                            self_gps.latitude_1e7,
                            local_heading_q,
                            &x_right_cm,
                            &y_front_cm);

    /*
     * 简单一阶低通滤波：如果航向来源没变，把本次结果与上一次做平均。
     * 这样可以在 GPS 抖动较大时让方向判断更稳定。
     */
    if ((peerTrack.filter_valid != 0U) && (peerTrack.heading_source == heading_source))
    {
      y_front_cm = (peerTrack.filtered_y_front_cm + y_front_cm) / 2;
      x_right_cm = (peerTrack.filtered_x_right_cm + x_right_cm) / 2;
    }
    peerTrack.filtered_y_front_cm = y_front_cm;
    peerTrack.filtered_x_right_cm = x_right_cm;

    /* 估算两车直线距离 */
    distance_cm = Bridge_DistanceApproxCm(y_front_cm, x_right_cm);

    /*
     * 距离趋势判断：
     * 当前距离 + 阈值 < 上次距离，说明确实在靠近，closing_count 累加。
     * 连续 2 次满足才认为趋势是 APPROACHING，避免单次噪声误触发。
     */
    if ((peerTrack.filter_valid != 0U) &&
        ((distance_cm + APP_BLE_BRIDGE_DISTANCE_TREND_THRESHOLD_CM) <
         peerTrack.last_distance_cm))
    {
      if (peerTrack.closing_count < 255U)
      {
        peerTrack.closing_count++;
      }
      if (peerTrack.closing_count >= 2U)
      {
        peerTrack.range_trend = APP_BLE_BRIDGE_RANGE_TREND_APPROACHING;
      }
    }
    /*
     * 距离明显变大：判定为 RECEDING（远离），清空 closing_count。
     * 注意：这里只需要 1 次满足就判远离，因为远离通常不需要太严格确认。
     */
    else if ((peerTrack.filter_valid != 0U) &&
             (distance_cm >
              (peerTrack.last_distance_cm + APP_BLE_BRIDGE_DISTANCE_TREND_THRESHOLD_CM)))
    {
      peerTrack.closing_count = 0U;
      peerTrack.range_trend = APP_BLE_BRIDGE_RANGE_TREND_RECEDING;
    }
    /* 距离变化在阈值以内：closing_count 递减，但不立即改变趋势 */
    else if (peerTrack.closing_count > 0U)
    {
      peerTrack.closing_count--;
    }

    /* 把趋势枚举转成布尔标志，方便后续条件判断 */
    closing = (peerTrack.range_trend == APP_BLE_BRIDGE_RANGE_TREND_APPROACHING) ? 1U : 0U;
    moving_away = (peerTrack.range_trend == APP_BLE_BRIDGE_RANGE_TREND_RECEDING) ? 1U : 0U;

    /* 保存本次距离和滤波状态，供下次趋势判断使用 */
    peerTrack.last_distance_cm = distance_cm;
    peerTrack.filter_valid = 1U;
    peerTrack.heading_source = heading_source;

    len = Bridge_AppendString(msg, len, sizeof(msg), "WARN: peer from ");
    len = Bridge_AppendString(msg, len, sizeof(msg), Bridge_DirectionText(y_front_cm, x_right_cm));
    if (closing != 0U)
    {
      len = Bridge_AppendString(msg, len, sizeof(msg), " approaching, distance decreasing");
    }
    else if (moving_away != 0U)
    {
      len = Bridge_AppendString(msg, len, sizeof(msg), " moving away, distance increasing");
    }
    else
    {
      len = Bridge_AppendString(msg, len, sizeof(msg), " relative trend unknown");
    }
    len = Bridge_AppendString(msg, len, sizeof(msg), "; distance~");
    len = Bridge_AppendUInt(msg, len, sizeof(msg), (distance_cm + 50U) / 100U);
    len = Bridge_AppendString(msg, len, sizeof(msg), "m, ");
    if (distance_cm <= APP_BLE_BRIDGE_TARGET_CLOSE_CM)
    {
      len = Bridge_AppendString(msg, len, sizeof(msg), "close");
    }
    else if (distance_cm <= APP_BLE_BRIDGE_TARGET_NEAR_CM)
    {
      len = Bridge_AppendString(msg, len, sizeof(msg), "nearby");
    }
    else
    {
      len = Bridge_AppendString(msg, len, sizeof(msg), "far");
    }
    len = Bridge_AppendString(msg, len, sizeof(msg), "; motion=");
    len = Bridge_AppendString(msg, len, sizeof(msg), Bridge_MotionText(status_flags));
    len = Bridge_AppendString(msg, len, sizeof(msg), "; heading_src=");
    if (heading_source == APP_BLE_BRIDGE_HEADING_SRC_IMU)
    {
      len = Bridge_AppendString(msg, len, sizeof(msg), "imu");
    }
    else if (self_gps.speed_cms >= APP_BLE_BRIDGE_GPS_COURSE_MIN_SPEED_CMS)
    {
      len = Bridge_AppendString(msg, len, sizeof(msg), "gps");
    }
    else
    {
      len = Bridge_AppendString(msg, len, sizeof(msg), "none");
    }
    len = Bridge_AppendString(msg, len, sizeof(msg), " y_front_cm=");
    len = Bridge_AppendInt(msg, len, sizeof(msg), y_front_cm);
    len = Bridge_AppendString(msg, len, sizeof(msg), " x_right_cm=");
    len = Bridge_AppendInt(msg, len, sizeof(msg), x_right_cm);
    len = Bridge_AppendString(msg, len, sizeof(msg), " peer_age_q=");
    len = Bridge_AppendUInt(msg, len, sizeof(msg), peerTrack.gps_age_q);
    len = Bridge_AppendString(msg, len, sizeof(msg), " self16=");
    len = Bridge_AppendUInt(msg, len, sizeof(msg), self_lat_low16);
    len = Bridge_AppendString(msg, len, sizeof(msg), ",");
    len = Bridge_AppendUInt(msg, len, sizeof(msg), self_lon_low16);
    len = Bridge_AppendString(msg, len, sizeof(msg), " peer16=");
    len = Bridge_AppendUInt(msg, len, sizeof(msg), peerTrack.lat_low16);
    len = Bridge_AppendString(msg, len, sizeof(msg), ",");
    len = Bridge_AppendUInt(msg, len, sizeof(msg), peerTrack.lon_low16);
    len = Bridge_AppendString(msg, len, sizeof(msg), " d_q=");
    len = Bridge_AppendInt(msg, len, sizeof(msg), dlat_q);
    len = Bridge_AppendString(msg, len, sizeof(msg), ",");
    len = Bridge_AppendInt(msg, len, sizeof(msg), dlon_q);
    len = Bridge_AppendString(msg, len, sizeof(msg), " heading_q=");
    len = Bridge_AppendUInt(msg, len, sizeof(msg), local_heading_q);
    len = Bridge_AppendString(msg, len, sizeof(msg), " imu_yaw_raw=");
    len = Bridge_AppendInt(msg, len, sizeof(msg), self_imu.yaw);
    len = Bridge_AppendString(msg, len, sizeof(msg), " imu_heading_cdeg=");
    len = Bridge_AppendInt(msg, len, sizeof(msg), Bridge_ImuYawToHeadingCdeg(self_imu.yaw));
    len = Bridge_AppendString(msg, len, sizeof(msg), " heading_offset_cdeg=");
    len = Bridge_AppendInt(msg, len, sizeof(msg), bridgeImuHeadingOffsetCdeg);
    len = Bridge_AppendString(msg, len, sizeof(msg), " heading_cal=");
    len = Bridge_AppendUInt(msg, len, sizeof(msg), bridgeHeadingCalValid);
    if (rssi_valid != 0U)
    {
      len = Bridge_AppendString(msg, len, sizeof(msg), " rssi=");
      len = Bridge_AppendInt(msg, len, sizeof(msg), rssi);
    }
    len = Bridge_AppendString(msg, len, sizeof(msg), "\r\n");
    Bridge_DebugPrint(msg);

    /*
     * 预警触发条件判断：
     * 1) 对端不是正在远离；
     * 2) 对端处于运动状态（移动/转向/刹车）；
     * 3) 距离在 30 米以内；
     * 4) 满足以下任一：正在靠近 / 对端标记高风险 / 距离在 8 米以内。
     *
     * cond_ok=1 时才会把候选语音交给防抖模块。
     */
    {
      APP_BLE_VoicePrompt_t selected_prompt = Bridge_SelectDirectionalVoicePrompt(y_front_cm,
                                                                                    x_right_cm);
      uint8_t cond_ok = ((selected_prompt != APP_BLE_VOICE_NONE) &&
                         (moving_away == 0U) &&
                         (Bridge_IsPeerMoving(status_flags) != 0U) &&
                         (distance_cm <= APP_BLE_BRIDGE_TARGET_NEAR_CM) &&
                         ((closing != 0U) ||
                          ((status_flags & APP_BLE_BRIDGE_STATUS_HIGH_RISK) != 0U) ||
                          (distance_cm <= APP_BLE_BRIDGE_TARGET_CLOSE_CM))) ? 1U : 0U;

      /* 输出预警判断来源，方便现场调试和参数整定 */
      len = 0U;
      len = Bridge_AppendString(msg, len, sizeof(msg), "ALERT_SRC: id=");
      len = Bridge_AppendUInt(msg, len, sizeof(msg), device_id);
      len = Bridge_AppendString(msg, len, sizeof(msg), " dist_cm=");
      len = Bridge_AppendUInt(msg, len, sizeof(msg), distance_cm);
      len = Bridge_AppendString(msg, len, sizeof(msg), " trend=");
      len = Bridge_AppendString(msg, len, sizeof(msg), Bridge_RangeTrendText(peerTrack.range_trend));
      len = Bridge_AppendString(msg, len, sizeof(msg), " cond_ok=");
      len = Bridge_AppendUInt(msg, len, sizeof(msg), cond_ok);
      len = Bridge_AppendString(msg, len, sizeof(msg), " moving=");
      len = Bridge_AppendUInt(msg, len, sizeof(msg), (Bridge_IsPeerMoving(status_flags) != 0U) ? 1U : 0U);
      len = Bridge_AppendString(msg, len, sizeof(msg), " closing=");
      len = Bridge_AppendUInt(msg, len, sizeof(msg), closing);
      len = Bridge_AppendString(msg, len, sizeof(msg), " moving_away=");
      len = Bridge_AppendUInt(msg, len, sizeof(msg), moving_away);
      len = Bridge_AppendString(msg, len, sizeof(msg), " near=");
      len = Bridge_AppendUInt(msg, len, sizeof(msg), (distance_cm <= APP_BLE_BRIDGE_TARGET_NEAR_CM) ? 1U : 0U);
      len = Bridge_AppendString(msg, len, sizeof(msg), " close=");
      len = Bridge_AppendUInt(msg, len, sizeof(msg), (distance_cm <= APP_BLE_BRIDGE_TARGET_CLOSE_CM) ? 1U : 0U);
      len = Bridge_AppendString(msg, len, sizeof(msg), " high_risk=");
      len = Bridge_AppendUInt(msg, len, sizeof(msg), ((status_flags & APP_BLE_BRIDGE_STATUS_HIGH_RISK) != 0U) ? 1U : 0U);
      len = Bridge_AppendString(msg, len, sizeof(msg), " y_front_cm=");
      len = Bridge_AppendInt(msg, len, sizeof(msg), y_front_cm);
      len = Bridge_AppendString(msg, len, sizeof(msg), " x_right_cm=");
      len = Bridge_AppendInt(msg, len, sizeof(msg), x_right_cm);
      len = Bridge_AppendString(msg, len, sizeof(msg), " prompt=");
      len = Bridge_AppendString(msg, len, sizeof(msg), Bridge_VoicePromptText(selected_prompt));
      len = Bridge_AppendString(msg, len, sizeof(msg), "\r\n");
      Bridge_InfoPrint(msg);

      /* 条件满足：把候选方向交给防抖模块，经过稳定时间和冷却后才真正播报 */
      if (cond_ok != 0U)
      {
        Bridge_UpdateVoiceCandidate(selected_prompt,
                                    device_id,
                                    distance_cm,
                                    y_front_cm,
                                    x_right_cm,
                                    now);
      }
      /* 条件不满足：取消未播放候选，并在持续安全后重新武装下一事件。 */
      else
      {
        Bridge_UpdateVoiceRearm(now);
      }
    }
  }
}

static void Bridge_CheckPeerTimeout(void)
{
  if ((peerTrack.valid != 0U) &&
      ((HAL_GetTick() - peerTrack.last_rx_ms) > APP_BLE_BRIDGE_PEER_STALE_MS))
  {
    peerTrack.valid = 0U;
    peerTrack.filter_valid = 0U;
    peerTrack.closing_count = 0U;
    peerTrack.range_trend = APP_BLE_BRIDGE_RANGE_TREND_UNKNOWN;
    Bridge_ResetVoiceEvent();
  }
}

static void Bridge_DebugPayload(const char *prefix, const uint8_t *payload, int8_t rssi, uint8_t rssi_valid)
{
  char msg[192] = {0};
  uint16_t len = 0U;
  uint8_t flags;

  if (payload == NULL)
  {
    return;
  }

  flags = payload[APP_BLE_BRIDGE_OFF_STATUS_FLAGS];
  len = Bridge_AppendString(msg, len, sizeof(msg), prefix);
  len = Bridge_AppendString(msg, len, sizeof(msg), ": id=");
  len = Bridge_AppendUInt(msg, len, sizeof(msg), Bridge_GetU16LE(&payload[APP_BLE_BRIDGE_OFF_DEVICE_ID]));
  len = Bridge_AppendString(msg, len, sizeof(msg), " cnt=");
  len = Bridge_AppendUInt(msg, len, sizeof(msg), payload[APP_BLE_BRIDGE_OFF_MSG_COUNTER]);
  len = Bridge_AppendString(msg, len, sizeof(msg), " flags=");
  len = Bridge_AppendUInt(msg, len, sizeof(msg), flags);
  len = Bridge_AppendString(msg, len, sizeof(msg), " bits=");
  len = Bridge_AppendString(msg, len, sizeof(msg), ((flags & APP_BLE_BRIDGE_STATUS_MOVING) != 0U) ? "M" : "-");
  len = Bridge_AppendString(msg, len, sizeof(msg), ((flags & APP_BLE_BRIDGE_STATUS_TURNING) != 0U) ? "T" : "-");
  len = Bridge_AppendString(msg, len, sizeof(msg), ((flags & APP_BLE_BRIDGE_STATUS_BRAKING) != 0U) ? "B" : "-");
  len = Bridge_AppendString(msg, len, sizeof(msg), ((flags & APP_BLE_BRIDGE_STATUS_GPS_VALID) != 0U) ? "G" : "-");
  len = Bridge_AppendString(msg, len, sizeof(msg), ((flags & APP_BLE_BRIDGE_STATUS_HIGH_RISK) != 0U) ? "R" : "-");
  len = Bridge_AppendString(msg, len, sizeof(msg), ((flags & APP_BLE_BRIDGE_STATUS_LOW_BATTERY) != 0U) ? "L" : "-");
  len = Bridge_AppendString(msg, len, sizeof(msg), ((flags & APP_BLE_BRIDGE_STATUS_IMU_VALID) != 0U) ? "I" : "-");
  len = Bridge_AppendString(msg, len, sizeof(msg), ((flags & APP_BLE_BRIDGE_STATUS_LOW_CONFIDENCE) != 0U) ? "C" : "-");
  len = Bridge_AppendString(msg, len, sizeof(msg), " gps=");
  len = Bridge_AppendUInt(msg, len, sizeof(msg), ((flags & APP_BLE_BRIDGE_STATUS_GPS_VALID) != 0U) ? 1U : 0U);
  len = Bridge_AppendString(msg, len, sizeof(msg), " speed_q=");
  len = Bridge_AppendUInt(msg, len, sizeof(msg), payload[APP_BLE_BRIDGE_OFF_SPEED_Q]);
  len = Bridge_AppendString(msg, len, sizeof(msg), " heading_q=");
  len = Bridge_AppendUInt(msg, len, sizeof(msg), Bridge_GetU16LE(&payload[APP_BLE_BRIDGE_OFF_HEADING_Q]));
  len = Bridge_AppendString(msg, len, sizeof(msg), " yaw_q=");
  len = Bridge_AppendInt(msg, len, sizeof(msg), (int8_t)payload[APP_BLE_BRIDGE_OFF_YAW_RATE_Q]);
  len = Bridge_AppendString(msg, len, sizeof(msg), " lat16=");
  len = Bridge_AppendUInt(msg, len, sizeof(msg), Bridge_GetU16LE(&payload[APP_BLE_BRIDGE_OFF_LAT_LOW16]));
  len = Bridge_AppendString(msg, len, sizeof(msg), " lon16=");
  len = Bridge_AppendUInt(msg, len, sizeof(msg), Bridge_GetU16LE(&payload[APP_BLE_BRIDGE_OFF_LON_LOW16]));
  len = Bridge_AppendString(msg, len, sizeof(msg), " q=");
  len = Bridge_AppendUInt(msg, len, sizeof(msg), payload[APP_BLE_BRIDGE_OFF_POS_QUALITY]);
  len = Bridge_AppendString(msg, len, sizeof(msg), " age_q=");
  len = Bridge_AppendUInt(msg, len, sizeof(msg), payload[APP_BLE_BRIDGE_OFF_GPS_AGE_Q]);
  len = Bridge_AppendString(msg, len, sizeof(msg), " gps_rmc_q=");
  len = Bridge_AppendUInt(msg, len, sizeof(msg),
                          payload[APP_BLE_BRIDGE_OFF_RESERVED] >> 4U);
  len = Bridge_AppendString(msg, len, sizeof(msg), " gps_fix_q=");
  len = Bridge_AppendUInt(msg, len, sizeof(msg),
                          payload[APP_BLE_BRIDGE_OFF_RESERVED] & 0x0FU);
  if (rssi_valid != 0U)
  {
    len = Bridge_AppendString(msg, len, sizeof(msg), " rssi=");
    len = Bridge_AppendInt(msg, len, sizeof(msg), rssi);
  }
  len = Bridge_AppendString(msg, len, sizeof(msg), " matches=");
  len = Bridge_AppendUInt(msg, len, sizeof(msg), filterMatchCount);
  len = Bridge_AppendString(msg, len, sizeof(msg), "\r\n");
  Bridge_DebugPrint(msg);
}

static int32_t Bridge_LongitudeScaleQ15(int32_t latitude_1e7)
{
  static const int32_t cos_q15_by_15deg[] =
  {
    32767, 31650, 28378, 23170, 16384, 8481, 0
  };
  uint32_t abs_lat;
  uint32_t deg;
  uint32_t idx;
  uint32_t rem;
  int32_t a;
  int32_t b;

  abs_lat = (latitude_1e7 < 0) ? (uint32_t)(-latitude_1e7) : (uint32_t)latitude_1e7;
  deg = abs_lat / 10000000U;
  if (deg >= 90U)
  {
    return 0;
  }

  idx = deg / 15U;
  rem = deg % 15U;
  if (idx >= 6U)
  {
    return 0;
  }

  a = cos_q15_by_15deg[idx];
  b = cos_q15_by_15deg[idx + 1U];
  return a + (int32_t)(((int64_t)(b - a) * rem) / 15);
}

static uint8_t Bridge_IsPeerMoving(uint8_t status_flags)
{
#if (APP_BLE_BRIDGE_FORCE_PEER_MOVING != 0U)
  /* 调试用：强制认为对端在动，用于验证语音链路和方向判断 */
  (void)status_flags;
  return 1U;
#else
  return (((status_flags & (APP_BLE_BRIDGE_STATUS_MOVING |
                           APP_BLE_BRIDGE_STATUS_TURNING |
                           APP_BLE_BRIDGE_STATUS_BRAKING)) != 0U) ?
          1U : 0U);
#endif
}

static uint8_t Bridge_QueueVoiceWarning(APP_BLE_VoicePrompt_t prompt,
                                        uint16_t device_id,
                                        uint32_t distance_cm,
                                        int32_t y_front_cm,
                                        int32_t x_right_cm)
{
  if ((prompt == APP_BLE_VOICE_NONE) ||
      (pendingVoiceWarning.prompt != APP_BLE_VOICE_NONE))
  {
    return 0U;
  }

  pendingVoiceWarning.prompt = prompt;
  pendingVoiceWarning.device_id = device_id;
  pendingVoiceWarning.distance_cm = distance_cm;
  pendingVoiceWarning.y_front_cm = y_front_cm;
  pendingVoiceWarning.x_right_cm = x_right_cm;
  return 1U;
}

static void Bridge_ResetVoiceCandidate(void)
{
  peerTrack.voice_candidate_prompt = APP_BLE_VOICE_NONE;
  peerTrack.voice_candidate_start_ms = 0U;
}

static void Bridge_CancelPendingVoiceWarning(void)
{
  Bridge_ResetVoiceCandidate();
  pendingVoiceWarning.prompt = APP_BLE_VOICE_NONE;
}

/* 完整清除一次语音风险事件；新设备和对端超时时立即调用。 */
static void Bridge_ResetVoiceEvent(void)
{
  Bridge_CancelPendingVoiceWarning();
  peerTrack.voice_last_prompt = APP_BLE_VOICE_NONE;
  bridgeVoiceReportValid = 0U;
  bridgeVoiceClearPending = 0U;
  bridgeVoiceClearStartMs = 0U;
}

/*
 * 风险必须连续解除一段时间才重新武装，避免GPS/RSSI单帧抖动把同一事件拆成多次。
 * 在锁存尚未形成时，条件不满足只需清空候选。
 */
static void Bridge_UpdateVoiceRearm(uint32_t now)
{
  Bridge_CancelPendingVoiceWarning();

  if (bridgeVoiceReportValid == 0U)
  {
    bridgeVoiceClearPending = 0U;
    bridgeVoiceClearStartMs = 0U;
    return;
  }

  if (bridgeVoiceClearPending == 0U)
  {
    bridgeVoiceClearPending = 1U;
    bridgeVoiceClearStartMs = now;
    return;
  }

  if ((now - bridgeVoiceClearStartMs) >= APP_BLE_BRIDGE_VOICE_REARM_HOLD_MS)
  {
    Bridge_InfoPrint("ALERT_REARM: risk cleared\r\n");
    Bridge_ResetVoiceEvent();
  }
}

/*
 * 语音候选防抖模块。
 * 不是每次满足条件都直接播报，而是要求：
 * 1) 同一个方向持续存在一段时间（VOICE_RISK_HOLD_MS）；
 * 2) 距离上次播报超过全局冷却时间（VOICE_REPORT_INTERVAL_MS）。
 * 首次播报后由事件锁存阻止同一连续风险再次进入本函数的入队阶段；只有风险稳定解除
 * 并重新武装后，新的候选才可能再次播报。
 */
static void Bridge_UpdateVoiceCandidate(APP_BLE_VoicePrompt_t prompt,
                                        uint16_t device_id,
                                        uint32_t distance_cm,
                                        int32_t y_front_cm,
                                        int32_t x_right_cm,
                                        uint32_t now)
{
  /* 风险仍在持续且本事件已经播报：保持锁存，不重复入队。 */
  bridgeVoiceClearPending = 0U;
  bridgeVoiceClearStartMs = 0U;
  if (bridgeVoiceReportValid != 0U)
  {
    Bridge_ResetVoiceCandidate();
    return;
  }

  /* 无有效方向时清空候选 */
  if (prompt == APP_BLE_VOICE_NONE)
  {
    Bridge_ResetVoiceCandidate();
    return;
  }

  /* 方向发生变化：重置稳定计时器，重新积累 */
  if (peerTrack.voice_candidate_prompt != prompt)
  {
    peerTrack.voice_candidate_prompt = prompt;
    peerTrack.voice_candidate_start_ms = now;
    return;
  }

  /* 方向稳定时间不足：继续等待 */
  if ((now - peerTrack.voice_candidate_start_ms) < APP_BLE_BRIDGE_VOICE_RISK_HOLD_MS)
  {
    return;
  }

  /* 全局冷却时间未过：避免语音连珠炮 */
  if ((now - bridgeVoiceLastReportMs) < APP_BLE_BRIDGE_VOICE_REPORT_INTERVAL_MS)
  {
    return;
  }

  /* 队列忙时不能打印“已入队”，避免日志给出错误结论。 */
  if (Bridge_QueueVoiceWarning(prompt, device_id, distance_cm,
                               y_front_cm, x_right_cm) == 0U)
  {
    return;
  }

  /* 打印最终被确认且成功入队的预警信息 */
  {
    char msg[128] = {0};
    uint16_t len = 0U;
    len = Bridge_AppendString(msg, len, sizeof(msg), "ALERT_QUEUE: id=");
    len = Bridge_AppendUInt(msg, len, sizeof(msg), device_id);
    len = Bridge_AppendString(msg, len, sizeof(msg), " prompt=");
    len = Bridge_AppendString(msg, len, sizeof(msg), Bridge_VoicePromptText(prompt));
    len = Bridge_AppendString(msg, len, sizeof(msg), " dist_cm=");
    len = Bridge_AppendUInt(msg, len, sizeof(msg), distance_cm);
    len = Bridge_AppendString(msg, len, sizeof(msg), " y_front_cm=");
    len = Bridge_AppendInt(msg, len, sizeof(msg), y_front_cm);
    len = Bridge_AppendString(msg, len, sizeof(msg), " x_right_cm=");
    len = Bridge_AppendInt(msg, len, sizeof(msg), x_right_cm);
    len = Bridge_AppendString(msg, len, sizeof(msg), " hold_ms=");
    len = Bridge_AppendUInt(msg, len, sizeof(msg), now - peerTrack.voice_candidate_start_ms);
    len = Bridge_AppendString(msg, len, sizeof(msg), "\r\n");
    Bridge_InfoPrint(msg);
  }

}

/*
 * 根据相对位置选择语音方向，并加入滞回（hysteresis）防止边界抖动。
 *
 * 坐标定义：
 *   y_front_cm > 0：目标在前方；< 0：目标在后方。
 *   x_right_cm > 0：目标在右侧；< 0：目标在左侧。
 *
 * 滞回设计：
 *   1) 左右方向：±200cm 死区，避免目标在正侧方时因 GPS 噪声左右跳变。
 *   2) 前后方向：从"侧方"进入"前方"需要 y >= 700cm；
 *               从"前方"退回"侧方"需要 y < 300cm。
 *      这样目标在 300~700cm 范围内不会前后跳变。
 *
 * 如果目标在后方 2m 以外，认为不需要提醒，返回 NONE。
 */
static APP_BLE_VoicePrompt_t Bridge_SelectDirectionalVoicePrompt(int32_t y_front_cm,
                                                                 int32_t x_right_cm)
{
  APP_BLE_VoicePrompt_t last_prompt = peerTrack.voice_last_prompt;
  uint8_t is_left;
  uint8_t is_front;
  APP_BLE_VoicePrompt_t prompt;

  /* 目标在后方 2m 外时不播报，并清除历史方向。 */
  if (y_front_cm < -200)
  {
    peerTrack.voice_last_prompt = APP_BLE_VOICE_NONE;
    return APP_BLE_VOICE_NONE;
  }

  /*
   * 左右方向滞回：
   * 上次方向是左时，x 必须越过 +200cm 才变右；
   * 上次方向是右时，x 必须越过 -200cm 才变左；
   * 没有历史方向时，按 0cm 硬边界判断。
   */
  if ((last_prompt == APP_BLE_VOICE_LEFT_RISK) ||
      (last_prompt == APP_BLE_VOICE_LEFT_FRONT_RISK))
  {
    is_left = (x_right_cm < 200) ? 1U : 0U;
  }
  else if ((last_prompt == APP_BLE_VOICE_RIGHT_RISK) ||
           (last_prompt == APP_BLE_VOICE_RIGHT_FRONT_RISK))
  {
    is_left = (x_right_cm < -200) ? 1U : 0U;
  }
  else
  {
    is_left = (x_right_cm < 0) ? 1U : 0U;
  }

  /*
   * 前后方向滞回：
   * 上次方向是前方时，y 必须低于 300cm 才退到侧方；
   * 上次方向是侧方时，y 必须高于 700cm 才进入前方。
   */
  if ((last_prompt == APP_BLE_VOICE_LEFT_FRONT_RISK) ||
      (last_prompt == APP_BLE_VOICE_RIGHT_FRONT_RISK))
  {
    is_front = (y_front_cm >= 300) ? 1U : 0U;
  }
  else
  {
    is_front = (y_front_cm >= 700) ? 1U : 0U;
  }

  /* 组合前后和左右，得到最终方向 */
  if (is_front != 0U)
  {
    prompt = (is_left != 0U) ? APP_BLE_VOICE_LEFT_FRONT_RISK :
                               APP_BLE_VOICE_RIGHT_FRONT_RISK;
  }
  else
  {
    prompt = (is_left != 0U) ? APP_BLE_VOICE_LEFT_RISK :
                               APP_BLE_VOICE_RIGHT_RISK;
  }

  /* 更新历史方向，供下次滞回判断使用 */
  peerTrack.voice_last_prompt = prompt;
  return prompt;
}

static const char *Bridge_MotionText(uint8_t status_flags)
{
  if ((status_flags & APP_BLE_BRIDGE_STATUS_BRAKING) != 0U)
  {
    return "braking";
  }
  if ((status_flags & APP_BLE_BRIDGE_STATUS_TURNING) != 0U)
  {
    return "turning";
  }
  if ((status_flags & APP_BLE_BRIDGE_STATUS_MOVING) != 0U)
  {
    return "moving";
  }

  return "still_or_unknown";
}

static const char *Bridge_RssiDistanceText(int8_t rssi, uint8_t rssi_valid)
{
  if (rssi_valid == 0U)
  {
    return "distance_unknown";
  }
  if (rssi >= -60)
  {
    return "close";
  }
  if (rssi >= -75)
  {
    return "nearby";
  }

  return "weak_nearby";
}

static const char *Bridge_DirectionText(int32_t y_front_cm, int32_t x_right_cm)
{
  int32_t abs_front = (y_front_cm < 0) ? -y_front_cm : y_front_cm;
  int32_t abs_right = (x_right_cm < 0) ? -x_right_cm : x_right_cm;

  if (abs_front < 200)
  {
    return (x_right_cm >= 0) ? "right" : "left";
  }
  if (abs_right < 200)
  {
    return (y_front_cm >= 0) ? "front" : "rear";
  }
  if (y_front_cm >= 0)
  {
    return (x_right_cm >= 0) ? "front_right" : "front_left";
  }

  return (x_right_cm >= 0) ? "rear_right" : "rear_left";
}

static const char *Bridge_VoicePromptText(APP_BLE_VoicePrompt_t prompt)
{
  switch (prompt)
  {
    case APP_BLE_VOICE_LEFT_RISK:
      return "left";
    case APP_BLE_VOICE_RIGHT_RISK:
      return "right";
    case APP_BLE_VOICE_LEFT_FRONT_RISK:
      return "left_front";
    case APP_BLE_VOICE_RIGHT_FRONT_RISK:
      return "right_front";
    case APP_BLE_VOICE_GPS_WEAK:
      return "gps_weak";
    case APP_BLE_VOICE_NONE:
    default:
      return "none";
  }
}

static const char *Bridge_RangeTrendText(uint8_t trend)
{
  switch (trend)
  {
    case APP_BLE_BRIDGE_RANGE_TREND_APPROACHING:
      return "approaching";
    case APP_BLE_BRIDGE_RANGE_TREND_RECEDING:
      return "receding";
    case APP_BLE_BRIDGE_RANGE_TREND_UNKNOWN:
    default:
      return "unknown";
  }
}

/*
 * 输出关键信息日志，不做节流。
 * 用于 ALERT_SRC / ALERT_QUEUE 等必须每次都能看到的事件。
 */
static void Bridge_InfoPrint(const char *text)
{
  uint16_t len = 0U;

  if (text == NULL)
  {
    return;
  }

  while ((text[len] != '\0') && (len < 360U))
  {
    len++;
  }

  if (len > 0U)
  {
    (void)HAL_UART_Transmit(&huart1, (uint8_t *)text, len, 100U);
  }
}

/*
 * 输出普通调试日志到 UART1，并做时间节流。
 * 普通日志最多每 APP_BLE_BRIDGE_DEBUG_PRINT_INTERVAL_MS 毫秒输出一次，
 * 避免 BLE 广播频率过高时刷爆串口。
 */
static void Bridge_DebugPrint(const char *text)
{
  uint16_t len = 0U;
  uint32_t now;

  if (text == NULL)
  {
    return;
  }

  now = HAL_GetTick();
  if ((now - bridgeDebugLastPrintMs) < APP_BLE_BRIDGE_DEBUG_PRINT_INTERVAL_MS)
  {
    return;
  }
  bridgeDebugLastPrintMs = now;

  while ((text[len] != '\0') && (len < 360U))
  {
    len++;
  }

  if (len > 0U)
  {
    (void)HAL_UART_Transmit(&huart1, (uint8_t *)text, len, 100U);
  }
}

static uint16_t Bridge_AppendString(char *dst, uint16_t pos, uint16_t size, const char *src)
{
  while ((src != NULL) && (*src != '\0') && ((pos + 1U) < size))
  {
    dst[pos++] = *src++;
    dst[pos] = '\0';
  }

  return pos;
}

static uint16_t Bridge_AppendUInt(char *dst, uint16_t pos, uint16_t size, uint32_t value)
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

static uint16_t Bridge_AppendInt(char *dst, uint16_t pos, uint16_t size, int32_t value)
{
  if ((value < 0) && ((pos + 1U) < size))
  {
    dst[pos++] = '-';
    dst[pos] = '\0';
    value = -value;
  }

  return Bridge_AppendUInt(dst, pos, size, (uint32_t)value);
}

/* USER CODE END FD_LOCAL_FUNCTION */

/*************************************************************
 *
 * WRAP FUNCTIONS
 *
 *************************************************************/

tBleStatus BLECB_Indication( const uint8_t* data,
                          uint16_t length,
                          const uint8_t* ext_data,
                          uint16_t ext_length )
{
  uint8_t status = BLE_STATUS_FAILED;
  BleEvtPacket_t *phcievt = NULL;
  uint16_t total_length = (length+ext_length);

  UNUSED(ext_data);

  if (data[0] == HCI_EVENT_PKT_TYPE)
  {
    APP_BLE_ResumeFlowProcessCb.Callback = BLE_ResumeFlowProcessCallback;
    if (AMM_Alloc (CFG_AMM_VIRTUAL_APP_BLE,
                   DIVC((sizeof(BleEvtPacketHeader_t) + total_length), sizeof (uint32_t)),
                   (uint32_t **)&phcievt,
                   &APP_BLE_ResumeFlowProcessCb) != AMM_ERROR_OK)
    {
      LOG_INFO_APP("Alloc failed\n");
      status = BLE_STATUS_FAILED;
    }
    else if (phcievt != (BleEvtPacket_t *)0 )
    {
      phcievt->evtserial.type = HCI_EVENT_PKT_TYPE;
      phcievt->evtserial.evt.evtcode = data[1];
      phcievt->evtserial.evt.plen  = data[2];
      MEMCPY( (void*)&phcievt->evtserial.evt.payload, &data[3], data[2]);
      LST_insert_tail(&BleAsynchEventQueue, (tListNode *)phcievt);
      UTIL_SEQ_SetTask(1U << CFG_TASK_HCI_ASYNCH_EVT_ID, CFG_SEQ_PRIO_0);
      status = BLE_STATUS_SUCCESS;
    }
  }
  else if (data[0] == HCI_ACLDATA_PKT_TYPE)
  {
    status = BLE_STATUS_SUCCESS;
  }
  return status;
}

/* USER CODE BEGIN FD_WRAP_FUNCTIONS */

/* USER CODE END FD_WRAP_FUNCTIONS */
