/**
 * ble.h — BLE auxiliary channel
 *
 * Advertises the node as a BLE peripheral so that a nearby phone
 * or secondary device can receive detection alerts without Wi-Fi.
 * Also provides a write characteristic for receiving commands when
 * the primary UDP channel is unavailable.
 */
#pragma once

/**
 * Initialise and start BLE advertising.
 *
 * @param node_id  Human-readable device name, e.g. "watch_left"
 */
void ble_init(const char *node_id);

/** Send a short notification string over BLE (max 20 bytes). */
void ble_notify(const char *message);

/** Stop BLE and release resources. */
void ble_deinit(void);
