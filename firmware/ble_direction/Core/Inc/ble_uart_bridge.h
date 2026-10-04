#ifndef BLE_UART_BRIDGE_H
#define BLE_UART_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "stm32wbaxx_hal.h"

void BLE_UART_Bridge_Init(UART_HandleTypeDef *huart);
void BLE_UART_Bridge_Process(void);
void BLE_UART_Bridge_OnUartRx(const uint8_t *data, uint16_t length);
void BLE_UART_Bridge_OnBleRx(const uint8_t *data, uint16_t length);
void BLE_UART_Bridge_SetLinkReady(uint8_t ready);
void BLE_UART_Bridge_Log(const char *message);

#ifdef __cplusplus
}
#endif

#endif /* BLE_UART_BRIDGE_H */
