/**
 * app_manager.h — App store client for Buddhas-Watch
 *
 * Connects to the self-hosted app store backend over HTTPS,
 * lists available apps, downloads and installs them to flash/SD.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * app_manager_start — Launch the app store LVGL task.
 * Requires Wi-Fi connectivity.
 */
void app_manager_start(void);

#ifdef __cplusplus
}
#endif
