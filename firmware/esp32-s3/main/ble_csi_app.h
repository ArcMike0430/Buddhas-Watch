/**
 * ble_csi_app.h — BLE GATT service for CSI streaming from Buddhas-Watch
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ble_csi_app_start — Initialise BLE stack and start the GATT CSI service.
 * Requires NVS and bt (Bluetooth) components to be initialised.
 */
void ble_csi_app_start(void);

#ifdef __cplusplus
}
#endif
