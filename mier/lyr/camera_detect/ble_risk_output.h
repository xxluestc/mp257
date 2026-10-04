#ifndef BLE_RISK_OUTPUT_H
#define BLE_RISK_OUTPUT_H

enum BleRiskState { BLE_RISK_CLEAR = 0, BLE_RISK_LEFT, BLE_RISK_CENTER, BLE_RISK_RIGHT };

/*
 * Non-blocking CH9140 UART output used by radar_fusion.
 * A failed/open-late UART is retried without stopping the radar main loop.
 */
void ble_risk_configure(const char *uart_device, bool enabled);
void ble_risk_update(BleRiskState state);
void ble_risk_shutdown(void);
const char *ble_risk_state_name(BleRiskState state);

#endif
