/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    A_app.c
  * @author  MCD Application Team
  * @brief   A_app application definition.
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
#include "app_ble.h"
#include "ll_sys_if.h"
#include "dbg_trace.h"
#include "ble.h"
#include "a_app.h"
#include "a.h"
#include "stm32_rtos.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

typedef enum
{
  B_NOTIFICATION_OFF,
  B_NOTIFICATION_ON,
  /* USER CODE BEGIN Service1_APP_SendInformation_t */

  /* USER CODE END Service1_APP_SendInformation_t */
  A_APP_SENDINFORMATION_LAST
} A_APP_SendInformation_t;

typedef struct
{
  A_APP_SendInformation_t     B_Notification_Status;
  /* USER CODE BEGIN Service1_APP_Context_t */

  /* USER CODE END Service1_APP_Context_t */
  uint16_t              ConnectionHandle;
} A_APP_Context_t;

/* Private defines -----------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* External variables --------------------------------------------------------*/
/* USER CODE BEGIN EV */

/* USER CODE END EV */

/* Private macros ------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
static A_APP_Context_t A_APP_Context;

uint8_t a_A_UpdateCharData[247];

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
static void A_B_SendNotification(void);

/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Functions Definition ------------------------------------------------------*/
void A_Notification(A_NotificationEvt_t *p_Notification)
{
  /* USER CODE BEGIN Service1_Notification_1 */

  /* USER CODE END Service1_Notification_1 */
  switch(p_Notification->EvtOpcode)
  {
    /* USER CODE BEGIN Service1_Notification_Service1_EvtOpcode */

    /* USER CODE END Service1_Notification_Service1_EvtOpcode */

    case A_B_NOTIFY_ENABLED_EVT:
      /* USER CODE BEGIN Service1Char1_NOTIFY_ENABLED_EVT */

      /* USER CODE END Service1Char1_NOTIFY_ENABLED_EVT */
      break;

    case A_B_NOTIFY_DISABLED_EVT:
      /* USER CODE BEGIN Service1Char1_NOTIFY_DISABLED_EVT */

      /* USER CODE END Service1Char1_NOTIFY_DISABLED_EVT */
      break;

    case A_C_WRITE_NO_RESP_EVT:
      /* USER CODE BEGIN Service1Char2_WRITE_NO_RESP_EVT */

      /* USER CODE END Service1Char2_WRITE_NO_RESP_EVT */
      break;

    default:
      /* USER CODE BEGIN Service1_Notification_default */

      /* USER CODE END Service1_Notification_default */
      break;
  }
  /* USER CODE BEGIN Service1_Notification_2 */

  /* USER CODE END Service1_Notification_2 */
  return;
}

void A_APP_EvtRx(A_APP_ConnHandleNotEvt_t *p_Notification)
{
  /* USER CODE BEGIN Service1_APP_EvtRx_1 */

  /* USER CODE END Service1_APP_EvtRx_1 */

  switch(p_Notification->EvtOpcode)
  {
    /* USER CODE BEGIN Service1_APP_EvtRx_Service1_EvtOpcode */

    /* USER CODE END Service1_APP_EvtRx_Service1_EvtOpcode */
    case A_CONN_HANDLE_EVT :
      /* USER CODE BEGIN Service1_APP_CONN_HANDLE_EVT */

      /* USER CODE END Service1_APP_CONN_HANDLE_EVT */
      break;

    case A_DISCON_HANDLE_EVT :
      /* USER CODE BEGIN Service1_APP_DISCON_HANDLE_EVT */

      /* USER CODE END Service1_APP_DISCON_HANDLE_EVT */
      break;

    default:
      /* USER CODE BEGIN Service1_APP_EvtRx_default */

      /* USER CODE END Service1_APP_EvtRx_default */
      break;
  }

  /* USER CODE BEGIN Service1_APP_EvtRx_2 */

  /* USER CODE END Service1_APP_EvtRx_2 */

  return;
}

void A_APP_Init(void)
{
  UNUSED(A_APP_Context);
  A_Init();

  /* USER CODE BEGIN Service1_APP_Init */

  /* USER CODE END Service1_APP_Init */
  return;
}

/* USER CODE BEGIN FD */

/* USER CODE END FD */

/*************************************************************
 *
 * LOCAL FUNCTIONS
 *
 *************************************************************/
__USED void A_B_SendNotification(void) /* Property Notification */
{
  A_APP_SendInformation_t notification_on_off = B_NOTIFICATION_OFF;
  A_Data_t a_notification_data;

  a_notification_data.p_Payload = (uint8_t*)a_A_UpdateCharData;
  a_notification_data.Length = 0;

  /* USER CODE BEGIN Service1Char1_NS_1 */

  /* USER CODE END Service1Char1_NS_1 */

  if (notification_on_off != B_NOTIFICATION_OFF)
  {
    A_UpdateValue(A_B, &a_notification_data);
  }

  /* USER CODE BEGIN Service1Char1_NS_Last */

  /* USER CODE END Service1Char1_NS_Last */

  return;
}

/* USER CODE BEGIN FD_LOCAL_FUNCTIONS */

/* EBYTE */
void user_notify(uint8_t * data, uint8_t length )
{
  A_Data_t a_notification_data;

  a_notification_data.p_Payload = data;
  a_notification_data.Length = length;
  
  A_UpdateValue(A_B, &a_notification_data);
}

/* USER CODE END FD_LOCAL_FUNCTIONS */
