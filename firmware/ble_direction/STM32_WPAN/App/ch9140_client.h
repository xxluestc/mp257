#ifndef CH9140_CLIENT_H
#define CH9140_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "ble.h"

void CH9140_Client_Init(void);
void CH9140_Client_OnConnected(uint16_t connection_handle);
void CH9140_Client_OnDisconnected(void);
void CH9140_Client_Process(void);
void CH9140_Client_LogStatus(void);
tBleStatus CH9140_Client_Write(const uint8_t *data, uint8_t length);
uint8_t CH9140_Client_IsReady(void);

#ifdef __cplusplus
}
#endif

#endif /* CH9140_CLIENT_H */
