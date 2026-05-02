/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_freertos.c
  * @author  MCD Application Team
  * @brief   app_freertos application implementation file
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2021 STMicroelectronics.
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
#include "app_freertos.h"
#include "portmacrocommon.h"
#include "cmsis_os2.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "FreeRTOS.h"
#include "task.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define MAX_BUFFER_SIZE RPMSG_BUFFER_SIZE

typedef struct {
    uint8_t data[MAX_BUFFER_SIZE];
    uint16_t len;
} Msg_t;
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */
VIRT_UART_HandleTypeDef huart0;

uint8_t VirtUart0ChannelBuffRx[MAX_BUFFER_SIZE];
uint16_t VirtUart0ChannelRxSize = 0;

static osSemaphoreId_t ShutDownSemaphore;
static const osSemaphoreAttr_t semAttr_ShutDownSemaphore = {
    .name = "ShutDownSemaphore",
};

static osThreadId_t ShutDownThreadHandle;
static const osThreadAttr_t ShutDown_attributes = {
  .name = "ShutDown Thread",
  .priority = (osPriority_t) osPriorityAboveNormal,
  .stack_size = 2 * 1024
};

static osThreadId_t M_MainTaskHandle;
static const osThreadAttr_t M_Main_attributes = {
  .name = "M_Main_Task",
  .priority = (osPriority_t) osPriorityNormal,
  .stack_size = 4096
};

static osThreadId_t OpenAMPTaskHandle;
static const osThreadAttr_t OpenAMP_attributes = {
  .name = "OpenAMP_Task",
  .priority = (osPriority_t) osPriorityHigh,
  .stack_size = 4096
};

static osThreadId_t M_RecvTaskHandle;
static const osThreadAttr_t M_Recv_attributes = {
  .name = "M_Recv_Task",
  .priority = (osPriority_t) osPriorityAboveNormal,
  .stack_size = 4096
};

static osThreadId_t M_SendTaskHandle;
static const osThreadAttr_t M_Send_attributes = {
  .name = "M_Send_Task",
  .priority = (osPriority_t) osPriorityAboveNormal,
  .stack_size = 4096
};

static osMessageQueueId_t RecvQueue;
static osMessageQueueId_t SendQueue;

static osSemaphoreId_t Semaphore;
static const osSemaphoreAttr_t semAttr_SEM1 = {
    .name = "SEM1",
};

static IPCC_HandleTypeDef const *hipcc_handle;
static uint32_t ipcc_ch_id;

volatile uint8_t messageReceived = 0;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */
static void M_Main_Task(void *argument);
static void OpenAMP_Task(void *argument);
static void M_Recv_Task(void *argument);
static void M_Send_Task(void *argument);
static void MX_FREERTOS_DeInit(void);
static void shutdown_thread_entry(void *argument);

void VIRT_UART0_RxCpltCallback(VIRT_UART_HandleTypeDef *huart);
/* USER CODE END PFP */

static void MX_FREERTOS_DeInit(void)
{
  HAL_DeInitTick();
  (void) osSemaphoreDelete(ShutDownSemaphore);
  (void) osSemaphoreDelete(Semaphore);
  (void) osMessageQueueDelete(RecvQueue);
  (void) osMessageQueueDelete(SendQueue);
  (void) osThreadTerminate(M_MainTaskHandle);
  (void) osThreadTerminate(OpenAMPTaskHandle);
  (void) osThreadTerminate(M_RecvTaskHandle);
  (void) osThreadTerminate(M_SendTaskHandle);
}

static void shutdown_thread_entry(void *argument)
{
  osSemaphoreAcquire(ShutDownSemaphore, osWaitForever);

  MX_FREERTOS_DeInit();

  MX_UART_DeInit();

  BSP_LED_DeInit(LED1);

  HAL_IPCC_NotifyCPU(hipcc_handle, ipcc_ch_id, IPCC_CHANNEL_DIR_RX);

  while(1);
}

void CoproSync_ShutdownCb(IPCC_HandleTypeDef * hipcc, uint32_t ChannelIndex, IPCC_CHANNELDirTypeDef ChannelDir)
{
  hipcc_handle = hipcc;
  ipcc_ch_id = ChannelIndex;
  (void) osSemaphoreRelease(ShutDownSemaphore);
}

void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  ShutDownSemaphore = osSemaphoreNew(1, 0, &semAttr_ShutDownSemaphore);
  if (NULL == ShutDownSemaphore)
  {
	Error_Handler();
  }

  Semaphore = osSemaphoreNew(1, 0, &semAttr_SEM1);
  if (NULL == Semaphore)
  {
    Error_Handler();
  }
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  RecvQueue = osMessageQueueNew(8, sizeof(Msg_t), NULL);
  if (NULL == RecvQueue)
  {
    Error_Handler();
  }

  SendQueue = osMessageQueueNew(8, sizeof(Msg_t), NULL);
  if (NULL == SendQueue)
  {
    Error_Handler();
  }
  /* USER CODE END RTOS_QUEUES */

  ShutDownThreadHandle = osThreadNew(shutdown_thread_entry, NULL, &ShutDown_attributes);
  if (NULL == ShutDownThreadHandle)
  {
    Error_Handler();
  }

  M_MainTaskHandle = osThreadNew(M_Main_Task, NULL, &M_Main_attributes);
  if (NULL == M_MainTaskHandle)
  {
    Error_Handler();
  }

  OpenAMPTaskHandle = osThreadNew(OpenAMP_Task, NULL, &OpenAMP_attributes);
  if (NULL == OpenAMPTaskHandle)
  {
    Error_Handler();
  }

  /* USER CODE BEGIN RTOS_THREADS */
  M_RecvTaskHandle = osThreadNew(M_Recv_Task, NULL, &M_Recv_attributes);
  if (NULL == M_RecvTaskHandle)
  {
    Error_Handler();
  }

  M_SendTaskHandle = osThreadNew(M_Send_Task, NULL, &M_Send_attributes);
  if (NULL == M_SendTaskHandle)
  {
    Error_Handler();
  }
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  loc_printf ("\r\n [M] Starting OpenAMP application (%s: %s) \r\n", __DATE__, __TIME__);
  MX_OPENAMP_Init(RPMSG_REMOTE, NULL);

  loc_printf("[M] Virtual UART0 OpenAMP-rpmsg channel creation\r\n");
  if (VIRT_UART_Init(&huart0) != VIRT_UART_OK) {
    loc_printf("[M] VIRT_UART_Init UART0 failed.\r\n");
    Error_Handler();
  }

  if(VIRT_UART_RegisterCallback(&huart0, VIRT_UART_RXCPLT_CB_ID, VIRT_UART0_RxCpltCallback) != VIRT_UART_OK)
  {
   Error_Handler();
  }

  loc_printf("[M] Waiting for rpmsg endpoint to be ready...\r\n");
  OPENAMP_Wait_EndPointready(&huart0.ept);
  loc_printf("[M] RPMSG endpoint ready!\r\n");

  /* USER CODE END RTOS_EVENTS */
}

static void M_Main_Task(void *argument)
{
    (void) argument;
    TickType_t xLastWakeTime = xTaskGetTickCount();

    loc_printf("[M-MAIN] Task started\r\n");

    while (1) {
        loc_printf("[M-MAIN] M-core main task is running...\r\n");
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(2000));
    }
}

static void OpenAMP_Task(void *argument)
{
    (void) argument;

    loc_printf("[OPENAMP] Task started\r\n");

    while (1) {
        OPENAMP_check_for_message();

        if (osOK == osSemaphoreAcquire(Semaphore, pdMS_TO_TICKS(100))) {
            Msg_t msg;
            msg.len = VirtUart0ChannelRxSize;
            memcpy(msg.data, VirtUart0ChannelBuffRx, msg.len);
            osMessageQueuePut(RecvQueue, &msg, 0, 0);
        }

        Msg_t tx_msg;
        while (osOK == osMessageQueueGet(SendQueue, &tx_msg, NULL, 0)) {
            VIRT_UART_TransmitNB(&huart0, tx_msg.data, tx_msg.len);
        }
    }
}

static void M_Recv_Task(void *argument)
{
    (void) argument;

    loc_printf("[M-RECV] Task started\r\n");

    while (1) {
        Msg_t msg;
        if (osOK == osMessageQueueGet(RecvQueue, &msg, NULL, osWaitForever)) {
            loc_printf("[M-RECV] <- A: \"%.*s\" (%d bytes)\r\n", msg.len, msg.data, msg.len);
        }
    }
}

static void M_Send_Task(void *argument)
{
    (void) argument;
    TickType_t xLastWakeTime = xTaskGetTickCount();
    int cycle = 0;

    loc_printf("[M-SEND] DVR trigger task started\r\n");

    while (1) {
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(5000));

        cycle++;
        loc_printf("[M-SEND] === Cycle %d ===\r\n", cycle);

        const char *cmd = NULL;
        int cmd_idx = (cycle - 1) % 3;

        switch (cmd_idx) {
        case 0: cmd = "TARGET_ON";  break;
        case 1: cmd = "WARNING";    break;
        case 2: cmd = "TARGET_OFF"; break;
        }

        Msg_t msg;
        msg.len = strlen(cmd);
        memcpy(msg.data, cmd, msg.len);

        if (osOK == osMessageQueuePut(SendQueue, &msg, 0, osWaitForever)) {
            loc_printf("[M-SEND] -> A: \"%s\" (%d bytes)\r\n", cmd, msg.len);
        }
    }
}

void VIRT_UART0_RxCpltCallback(VIRT_UART_HandleTypeDef *huart)
{
    VirtUart0ChannelRxSize = huart->RxXferSize < MAX_BUFFER_SIZE ? huart->RxXferSize : MAX_BUFFER_SIZE - 1;

    memset(VirtUart0ChannelBuffRx, 0, MAX_BUFFER_SIZE);
    memcpy(VirtUart0ChannelBuffRx, huart->pRxBuffPtr, VirtUart0ChannelRxSize);

    messageReceived = 1;

    if (osOK == osSemaphoreRelease(Semaphore)) {
    	BSP_LED_Toggle(LED1);
    }
}
