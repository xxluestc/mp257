#include "ble_uart_bridge.h"

#include <string.h>

#include "ch9140_client.h"
#include "ble.h"

#define BLE_UART_RING_SIZE          512U
#define BLE_UART_RING_MASK          (BLE_UART_RING_SIZE - 1U)
#define BLE_UART_NOTIFY_SIZE        20U
#define BLE_UART_TX_CHUNK_SIZE      64U
#define BLE_UART_TX_TIMEOUT_MS      100U

typedef struct
{
  uint8_t data[BLE_UART_RING_SIZE];
  volatile uint16_t head;
  volatile uint16_t tail;
  volatile uint32_t dropped;
} ByteRing_t;

static UART_HandleTypeDef *bridge_uart;
static ByteRing_t uart_to_ble;
static ByteRing_t ble_to_uart;
static volatile uint8_t link_ready;

static uint16_t RingNext(uint16_t index)
{
  return (uint16_t)((index + 1U) & BLE_UART_RING_MASK);
}

static void RingClear(ByteRing_t *ring)
{
  ring->tail = ring->head;
}

static void RingPush(ByteRing_t *ring, const uint8_t *data, uint16_t length)
{
  uint16_t i;

  if (data == NULL)
  {
    return;
  }

  for (i = 0U; i < length; i++)
  {
    uint16_t next = RingNext(ring->head);

    if (next == ring->tail)
    {
      ring->dropped++;
      continue;
    }

    ring->data[ring->head] = data[i];
    ring->head = next;
  }
}

static uint16_t RingPeek(const ByteRing_t *ring, uint8_t *data, uint16_t capacity,
                         uint16_t *next_tail)
{
  uint16_t count = 0U;
  uint16_t index = ring->tail;

  while ((index != ring->head) && (count < capacity))
  {
    data[count++] = ring->data[index];
    index = RingNext(index);
  }

  *next_tail = index;
  return count;
}

void BLE_UART_Bridge_Init(UART_HandleTypeDef *huart)
{
  bridge_uart = huart;
  uart_to_ble.head = 0U;
  uart_to_ble.tail = 0U;
  uart_to_ble.dropped = 0U;
  ble_to_uart.head = 0U;
  ble_to_uart.tail = 0U;
  ble_to_uart.dropped = 0U;
  link_ready = 0U;
}

void BLE_UART_Bridge_SetLinkReady(uint8_t ready)
{
  static const uint8_t ready_message[] = "\r\n[BLE] CH9140 link ready\r\n";
  static const uint8_t down_message[] = "\r\n[BLE] CH9140 link down\r\n";

  link_ready = (ready != 0U) ? 1U : 0U;

  if (link_ready == 0U)
  {
    RingClear(&uart_to_ble);
    RingPush(&ble_to_uart, down_message,
             (uint16_t)(sizeof(down_message) - 1U));
  }
  else
  {
    RingPush(&ble_to_uart, ready_message,
             (uint16_t)(sizeof(ready_message) - 1U));
  }
}

void BLE_UART_Bridge_OnUartRx(const uint8_t *data, uint16_t length)
{
  if (link_ready == 0U)
  {
    uart_to_ble.dropped += length;
    return;
  }

  RingPush(&uart_to_ble, data, length);
}

void BLE_UART_Bridge_OnBleRx(const uint8_t *data, uint16_t length)
{
  RingPush(&ble_to_uart, data, length);
}

void BLE_UART_Bridge_Log(const char *message)
{
  size_t length;

  if (message == NULL)
  {
    return;
  }

  length = strlen(message);
  if (length > 0xFFFFU)
  {
    length = 0xFFFFU;
  }
  RingPush(&ble_to_uart, (const uint8_t *)message, (uint16_t)length);
}

void BLE_UART_Bridge_Process(void)
{
  uint8_t buffer[BLE_UART_TX_CHUNK_SIZE];
  uint16_t next_tail;
  uint16_t length;

  if (bridge_uart != NULL)
  {
    length = RingPeek(&ble_to_uart, buffer, sizeof(buffer), &next_tail);
    if (length > 0U)
    {
      if (HAL_UART_Transmit(bridge_uart, buffer, length,
                            BLE_UART_TX_TIMEOUT_MS) == HAL_OK)
      {
        ble_to_uart.tail = next_tail;
      }
    }
  }

  if (link_ready != 0U)
  {
    length = RingPeek(&uart_to_ble, buffer, BLE_UART_NOTIFY_SIZE, &next_tail);
    if (length > 0U)
    {
      if (CH9140_Client_Write(buffer, (uint8_t)length) == BLE_STATUS_SUCCESS)
      {
        uart_to_ble.tail = next_tail;
      }
    }
  }
}
