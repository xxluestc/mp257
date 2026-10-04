/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    A.h
  * @author  MCD Application Team
  * @brief   Header for A.c
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
#ifndef A_H
#define A_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported defines ----------------------------------------------------------*/
/* USER CODE BEGIN ED */

/* USER CODE END ED */

/* Exported types ------------------------------------------------------------*/
typedef enum
{
  A_B,
  A_C,
  /* USER CODE BEGIN Service1_CharOpcode_t */

  /* USER CODE END Service1_CharOpcode_t */
  A_CHAROPCODE_LAST
} A_CharOpcode_t;

typedef enum
{
  A_B_NOTIFY_ENABLED_EVT,
  A_B_NOTIFY_DISABLED_EVT,
  A_C_WRITE_NO_RESP_EVT,
  /* USER CODE BEGIN Service1_OpcodeEvt_t */

  /* USER CODE END Service1_OpcodeEvt_t */
  A_BOOT_REQUEST_EVT
} A_OpcodeEvt_t;

typedef struct
{
  uint8_t *p_Payload;
  uint8_t Length;

  /* USER CODE BEGIN Service1_Data_t */

  /* USER CODE END Service1_Data_t */
} A_Data_t;

typedef struct
{
  A_OpcodeEvt_t       EvtOpcode;
  A_Data_t             DataTransfered;
  uint16_t                ConnectionHandle;
  uint16_t                AttributeHandle;
  uint8_t                 ServiceInstance;
  /* USER CODE BEGIN Service1_NotificationEvt_t */

  /* USER CODE END Service1_NotificationEvt_t */
} A_NotificationEvt_t;

/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* External variables --------------------------------------------------------*/
/* USER CODE BEGIN EV */

/* USER CODE END EV */

/* Exported macros -----------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void A_Init(void);
void A_Notification(A_NotificationEvt_t *p_Notification);
tBleStatus A_UpdateValue(A_CharOpcode_t CharOpcode, A_Data_t *pData);
/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

#ifdef __cplusplus
}
#endif

#endif /*A_H */
