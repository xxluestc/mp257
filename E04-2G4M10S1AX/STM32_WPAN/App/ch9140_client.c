#include "ch9140_client.h"

#include <string.h>

#include "ble_uart_bridge.h"
#include "log_module.h"
#include "main.h"
#include "svc_ctl.h"

#define CH9140_SERVICE_UUID       0xFFF0U
#define CH9140_NOTIFY_UUID        0xFFF1U
#define CH9140_WRITE_UUID         0xFFF2U
#define CLIENT_CONFIG_UUID        0x2902U
#define CH9140_MAX_PAYLOAD        20U
#define COMMAND_BUFFER_SIZE       64U
#define REPLY_BUFFER_SIZE         64U

#define UNPACK_U16(ptr) \
  ((uint16_t)((uint16_t)(ptr)[0] | ((uint16_t)(ptr)[1] << 8)))

typedef enum
{
  CH9140_DISCONNECTED = 0,
  CH9140_DISCOVERING_SERVICE,
  CH9140_DISCOVERING_CHARACTERISTICS,
  CH9140_DISCOVERING_DESCRIPTORS,
  CH9140_ENABLING_NOTIFY,
  CH9140_READY
} CH9140_State_t;

typedef struct
{
  CH9140_State_t state;
  uint16_t connection_handle;
  uint16_t service_start;
  uint16_t service_end;
  uint16_t notify_value;
  uint16_t write_value;
  uint16_t write_declaration;
  uint16_t notify_cccd;
  uint8_t command[COMMAND_BUFFER_SIZE];
  uint8_t command_length;
  uint8_t reply[REPLY_BUFFER_SIZE];
  uint8_t reply_length;
} CH9140_Context_t;

static CH9140_Context_t client;

static SVCCTL_EvtAckStatus_t CH9140_EventHandler(void *event);
static void CH9140_ParseCommandBytes(const uint8_t *data, uint8_t length);
static void CH9140_QueueReply(const char *text);
static void CH9140_SetLed(uint8_t enabled);
static void CH9140_SetLeftLed(uint8_t enabled);
static void CH9140_SetRightLed(uint8_t enabled);
static void CH9140_SetRiskLeds(uint8_t left, uint8_t right);
static void CH9140_Fail(const char *stage, tBleStatus status);

void CH9140_Client_Init(void)
{
  memset(&client, 0, sizeof(client));
  client.state = CH9140_DISCONNECTED;
  client.connection_handle = 0xFFFFU;
  CH9140_SetLed(0U);
  CH9140_SetRiskLeds(0U, 0U);
  SVCCTL_RegisterCltHandler(CH9140_EventHandler);
}

void CH9140_Client_OnConnected(uint16_t connection_handle)
{
  UUID_t uuid;
  tBleStatus status;

  memset(&client, 0, sizeof(client));
  client.connection_handle = connection_handle;
  client.state = CH9140_DISCOVERING_SERVICE;
  uuid.UUID_16 = CH9140_SERVICE_UUID;

  BLE_UART_Bridge_SetLinkReady(0U);
  BLE_UART_Bridge_Log("[GATT] discover FFF0 service\r\n");
  status = aci_gatt_disc_primary_service_by_uuid(connection_handle,
                                                  UUID_TYPE_16,
                                                  &uuid);
  if (status != BLE_STATUS_SUCCESS)
  {
    CH9140_Fail("service discovery start", status);
  }
}

void CH9140_Client_OnDisconnected(void)
{
  client.state = CH9140_DISCONNECTED;
  client.connection_handle = 0xFFFFU;
  client.reply_length = 0U;
  client.command_length = 0U;
  BLE_UART_Bridge_SetLinkReady(0U);
  /* Fail safe: a stale collision indication must not remain lit. */
  CH9140_SetRiskLeds(0U, 0U);
}

uint8_t CH9140_Client_IsReady(void)
{
  return (client.state == CH9140_READY) ? 1U : 0U;
}

tBleStatus CH9140_Client_Write(const uint8_t *data, uint8_t length)
{
  if ((client.state != CH9140_READY) || (data == NULL) ||
      (length == 0U) || (length > CH9140_MAX_PAYLOAD))
  {
    return BLE_STATUS_INVALID_PARAMS;
  }

  return aci_gatt_write_without_resp(client.connection_handle,
                                     client.write_value,
                                     length,
                                     data);
}

void CH9140_Client_Process(void)
{
  tBleStatus status;
  uint8_t length;

  if ((client.state != CH9140_READY) || (client.reply_length == 0U))
  {
    return;
  }

  length = client.reply_length;
  if (length > CH9140_MAX_PAYLOAD)
  {
    length = CH9140_MAX_PAYLOAD;
  }

  status = CH9140_Client_Write(client.reply, length);
  if (status == BLE_STATUS_SUCCESS)
  {
    client.reply_length = (uint8_t)(client.reply_length - length);
    if (client.reply_length > 0U)
    {
      memmove(client.reply, &client.reply[length], client.reply_length);
    }
  }
}

void CH9140_Client_LogStatus(void)
{
  switch (client.state)
  {
    case CH9140_DISCOVERING_SERVICE:
      BLE_UART_Bridge_Log("[STATUS] GAP=connected GATT=discover-FFF0\r\n");
      break;
    case CH9140_DISCOVERING_CHARACTERISTICS:
      BLE_UART_Bridge_Log("[STATUS] GAP=connected GATT=discover-FFF1-FFF2\r\n");
      break;
    case CH9140_DISCOVERING_DESCRIPTORS:
      BLE_UART_Bridge_Log("[STATUS] GAP=connected GATT=discover-CCCD\r\n");
      break;
    case CH9140_ENABLING_NOTIFY:
      BLE_UART_Bridge_Log("[STATUS] GAP=connected GATT=enable-notify\r\n");
      break;
    case CH9140_READY:
      BLE_UART_Bridge_Log("[STATUS] GAP=connected GATT=ready\r\n");
      break;
    default:
      BLE_UART_Bridge_Log("[STATUS] GAP=connected GATT=idle/error\r\n");
      break;
  }
}

static void CH9140_Fail(const char *stage, tBleStatus status)
{
  LOG_INFO_APP("CH9140 client: %s failed, status=0x%02X\n", stage, status);
  BLE_UART_Bridge_Log("[GATT] ERROR: ");
  BLE_UART_Bridge_Log(stage);
  BLE_UART_Bridge_Log("; disconnect and retry\r\n");
  (void)stage;
  (void)status;
  client.state = CH9140_DISCONNECTED;
  BLE_UART_Bridge_SetLinkReady(0U);
  if (client.connection_handle != 0xFFFFU)
  {
    (void)aci_gap_terminate(client.connection_handle,
                            HCI_REMOTE_USER_TERMINATED_CONNECTION_ERR_CODE);
  }
}

static void CH9140_SetLed(uint8_t enabled)
{
  /* EWT04 D1 is wired to PA2 and is active low. */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2,
                    (enabled != 0U) ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static void CH9140_SetLeftLed(uint8_t enabled)
{
  /* External LEFT LED: PA7 -> resistor -> LED -> GND, active high. */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7,
                    (enabled != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void CH9140_SetRightLed(uint8_t enabled)
{
  /* External RIGHT LED: PA5 -> resistor -> LED -> GND, active high. */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5,
                    (enabled != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void CH9140_SetRiskLeds(uint8_t left, uint8_t right)
{
  CH9140_SetLeftLed(left);
  CH9140_SetRightLed(right);
}

static void CH9140_QueueReply(const char *text)
{
  size_t length;

  if (text == NULL)
  {
    return;
  }

  length = strlen(text);
  if (length > sizeof(client.reply))
  {
    length = sizeof(client.reply);
  }

  memcpy(client.reply, text, length);
  client.reply_length = (uint8_t)length;
}

static void CH9140_ParseCommandBytes(const uint8_t *data, uint8_t length)
{
  uint8_t i;

  for (i = 0U; i < length; i++)
  {
    uint8_t byte = data[i];

    if ((byte == '\r') || (byte == '\n'))
    {
      if (client.command_length == 0U)
      {
        continue;
      }

      client.command[client.command_length] = '\0';
      if (strcmp((char *)client.command, "LED ON") == 0)
      {
        CH9140_SetLed(1U);
        CH9140_QueueReply("ACK LED ON\n");
      }
      else if (strcmp((char *)client.command, "LED OFF") == 0)
      {
        CH9140_SetLed(0U);
        CH9140_QueueReply("ACK LED OFF\n");
      }
      else if (strcmp((char *)client.command, "LEFT ON") == 0)
      {
        CH9140_SetLeftLed(1U);
        CH9140_QueueReply("ACK LEFT ON\n");
      }
      else if (strcmp((char *)client.command, "LEFT OFF") == 0)
      {
        CH9140_SetLeftLed(0U);
        CH9140_QueueReply("ACK LEFT OFF\n");
      }
      else if (strcmp((char *)client.command, "RIGHT ON") == 0)
      {
        CH9140_SetRightLed(1U);
        CH9140_QueueReply("ACK RIGHT ON\n");
      }
      else if (strcmp((char *)client.command, "RIGHT OFF") == 0)
      {
        CH9140_SetRightLed(0U);
        CH9140_QueueReply("ACK RIGHT OFF\n");
      }
      else if (strcmp((char *)client.command, "RISK LEFT") == 0)
      {
        CH9140_SetRiskLeds(1U, 0U);
        CH9140_QueueReply("ACK RISK LEFT\n");
      }
      else if (strcmp((char *)client.command, "RISK RIGHT") == 0)
      {
        CH9140_SetRiskLeds(0U, 1U);
        CH9140_QueueReply("ACK RISK RIGHT\n");
      }
      else if (strcmp((char *)client.command, "RISK CENTER") == 0)
      {
        CH9140_SetRiskLeds(1U, 1U);
        CH9140_QueueReply("ACK RISK CENTER\n");
      }
      else if (strcmp((char *)client.command, "RISK CLEAR") == 0)
      {
        CH9140_SetRiskLeds(0U, 0U);
        CH9140_QueueReply("ACK RISK CLEAR\n");
      }
      else if (strcmp((char *)client.command, "PING") == 0)
      {
        CH9140_QueueReply("PONG\n");
      }
      else
      {
        CH9140_QueueReply("ERR UNKNOWN CMD\n");
      }
      client.command_length = 0U;
      continue;
    }

    if (client.command_length < (COMMAND_BUFFER_SIZE - 1U))
    {
      client.command[client.command_length++] = byte;
    }
    else
    {
      client.command_length = 0U;
      CH9140_QueueReply("ERR CMD TOO LONG\n");
    }
  }
}

static SVCCTL_EvtAckStatus_t CH9140_EventHandler(void *event)
{
  hci_event_pckt *event_packet;
  evt_blecore_aci *aci_event;
  tBleStatus status;

  event_packet = (hci_event_pckt *)(((hci_uart_pckt *)event)->data);
  if (event_packet->evt != HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE)
  {
    return SVCCTL_EvtNotAck;
  }

  aci_event = (evt_blecore_aci *)event_packet->data;
  switch (aci_event->ecode)
  {
    case ACI_ATT_FIND_BY_TYPE_VALUE_RESP_VSEVT_CODE:
    {
      aci_att_find_by_type_value_resp_event_rp0 *response =
        (aci_att_find_by_type_value_resp_event_rp0 *)aci_event->data;

      if ((client.state == CH9140_DISCOVERING_SERVICE) &&
          (response->Connection_Handle == client.connection_handle) &&
          (response->Num_of_Handle_Pair > 0U))
      {
        client.service_start =
          response->Attribute_Group_Handle_Pair[0].Found_Attribute_Handle;
        client.service_end =
          response->Attribute_Group_Handle_Pair[0].Group_End_Handle;
      }
      break;
    }

    case ACI_ATT_READ_BY_TYPE_RESP_VSEVT_CODE:
    {
      aci_att_read_by_type_resp_event_rp0 *response =
        (aci_att_read_by_type_resp_event_rp0 *)aci_event->data;
      uint8_t offset;

      if ((client.state != CH9140_DISCOVERING_CHARACTERISTICS) ||
          (response->Connection_Handle != client.connection_handle) ||
          (response->Handle_Value_Pair_Length != 7U))
      {
        break;
      }

      for (offset = 0U;
           (uint16_t)(offset + 7U) <= response->Data_Length;
           offset = (uint8_t)(offset + 7U))
      {
        uint8_t *tuple = &response->Handle_Value_Pair_Data[offset];
        uint16_t declaration = UNPACK_U16(tuple);
        uint16_t value = UNPACK_U16(&tuple[3]);
        uint16_t uuid = UNPACK_U16(&tuple[5]);

        if (uuid == CH9140_NOTIFY_UUID)
        {
          client.notify_value = value;
        }
        else if (uuid == CH9140_WRITE_UUID)
        {
          client.write_declaration = declaration;
          client.write_value = value;
        }
      }
      break;
    }

    case ACI_ATT_FIND_INFO_RESP_VSEVT_CODE:
    {
      aci_att_find_info_resp_event_rp0 *response =
        (aci_att_find_info_resp_event_rp0 *)aci_event->data;
      uint8_t offset;

      if ((client.state != CH9140_DISCOVERING_DESCRIPTORS) ||
          (response->Connection_Handle != client.connection_handle) ||
          (response->Format != UUID_TYPE_16))
      {
        break;
      }

      for (offset = 0U;
           (uint16_t)(offset + 4U) <= response->Event_Data_Length;
           offset = (uint8_t)(offset + 4U))
      {
        uint8_t *tuple = &response->Handle_UUID_Pair[offset];
        if (UNPACK_U16(&tuple[2]) == CLIENT_CONFIG_UUID)
        {
          client.notify_cccd = UNPACK_U16(tuple);
        }
      }
      break;
    }

    case ACI_GATT_NOTIFICATION_VSEVT_CODE:
    {
      aci_gatt_notification_event_rp0 *notification =
        (aci_gatt_notification_event_rp0 *)aci_event->data;

      if ((client.state == CH9140_READY) &&
          (notification->Connection_Handle == client.connection_handle) &&
          (notification->Attribute_Handle == client.notify_value))
      {
        BLE_UART_Bridge_OnBleRx(notification->Attribute_Value,
                               notification->Attribute_Value_Length);
        CH9140_ParseCommandBytes(notification->Attribute_Value,
                                notification->Attribute_Value_Length);
        return SVCCTL_EvtAckFlowEnable;
      }
      break;
    }

    case ACI_GATT_PROC_COMPLETE_VSEVT_CODE:
    {
      aci_gatt_proc_complete_event_rp0 *complete =
        (aci_gatt_proc_complete_event_rp0 *)aci_event->data;

      if (complete->Connection_Handle != client.connection_handle)
      {
        break;
      }

      switch (client.state)
      {
        case CH9140_DISCOVERING_SERVICE:
          if (client.service_start == 0U)
          {
            CH9140_Fail("FFF0 service not found", complete->Error_Code);
            break;
          }
          client.state = CH9140_DISCOVERING_CHARACTERISTICS;
          BLE_UART_Bridge_Log("[GATT] FFF0 found; discover FFF1/FFF2\r\n");
          status = aci_gatt_disc_all_char_of_service(client.connection_handle,
                                                      client.service_start,
                                                      client.service_end);
          if (status != BLE_STATUS_SUCCESS)
          {
            CH9140_Fail("characteristic discovery start", status);
          }
          break;

        case CH9140_DISCOVERING_CHARACTERISTICS:
          if ((client.notify_value == 0U) || (client.write_value == 0U))
          {
            CH9140_Fail("FFF1/FFF2 not found", complete->Error_Code);
            break;
          }
          client.state = CH9140_DISCOVERING_DESCRIPTORS;
          BLE_UART_Bridge_Log("[GATT] FFF1/FFF2 found; discover CCCD\r\n");
          status = aci_gatt_disc_all_char_desc(
            client.connection_handle,
            client.notify_value,
            (client.write_declaration > client.notify_value)
              ? (uint16_t)(client.write_declaration - 1U)
              : client.service_end);
          if (status != BLE_STATUS_SUCCESS)
          {
            CH9140_Fail("descriptor discovery start", status);
          }
          break;

        case CH9140_DISCOVERING_DESCRIPTORS:
        {
          const uint8_t enable_notify[2] = { 0x01U, 0x00U };

          if (client.notify_cccd == 0U)
          {
            CH9140_Fail("FFF1 CCCD not found", complete->Error_Code);
            break;
          }
          client.state = CH9140_ENABLING_NOTIFY;
          BLE_UART_Bridge_Log("[GATT] CCCD found; enable FFF1 notify\r\n");
          status = aci_gatt_write_char_desc(client.connection_handle,
                                            client.notify_cccd,
                                            sizeof(enable_notify),
                                            enable_notify);
          if (status != BLE_STATUS_SUCCESS)
          {
            CH9140_Fail("enable FFF1 notify", status);
          }
          break;
        }

        case CH9140_ENABLING_NOTIFY:
          if (complete->Error_Code == BLE_STATUS_SUCCESS)
          {
            client.state = CH9140_READY;
            BLE_UART_Bridge_SetLinkReady(1U);
            BLE_UART_Bridge_Log("[GATT] transparent link READY\r\n");
            LOG_INFO_APP("CH9140 client: link ready\n");
          }
          else
          {
            CH9140_Fail("enable FFF1 notify complete",
                        complete->Error_Code);
          }
          break;

        default:
          break;
      }
      break;
    }

    default:
      break;
  }

  return SVCCTL_EvtNotAck;
}
