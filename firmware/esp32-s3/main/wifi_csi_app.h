/**
 * wifi_csi_app.h — Wi-Fi CSI capture, LVGL heatmap display, UDP streaming
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * wifi_csi_app_start — Launch the Wi-Fi CSI capture and display task.
 * Requires Wi-Fi to be initialised and connected before calling.
 */
void wifi_csi_app_start(void);

#ifdef __cplusplus
}
#endif
