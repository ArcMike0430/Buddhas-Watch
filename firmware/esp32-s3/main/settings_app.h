/**
 * settings_app.h — Persistent device configuration for Buddhas-Watch
 *
 * Provides NVS-backed configuration storage and an LVGL settings UI.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Configuration structure stored in NVS */
typedef struct {
    char     wifi_ssid[64];
    char     wifi_password[64];
    char     node_id[32];
    char     jetson_ip[16];
    uint16_t udp_port;
    uint8_t  display_brightness;  /* 0–255                                  */
    uint16_t display_timeout_s;   /* Screen-off timeout in seconds          */
    bool     ble_enabled;
    bool     logging_to_sd;
    bool     wifi_streaming;
    bool     auto_update;         /* Auto-check for app store updates       */
} buddhas_config_t;

/**
 * settings_app_start — Launch the settings LVGL task.
 * Must be called after NVS is initialised.
 */
void settings_app_start(void);

/**
 * settings_load — Load configuration from NVS into the provided struct.
 * Fills defaults for any keys that are not yet present.
 */
void settings_load(buddhas_config_t *cfg);

/**
 * settings_save — Persist the provided configuration struct to NVS.
 */
void settings_save(const buddhas_config_t *cfg);

/**
 * settings_get — Return a const pointer to the in-memory config.
 * Valid after settings_app_start() has been called.
 */
const buddhas_config_t *settings_get(void);

#ifdef __cplusplus
}
#endif
