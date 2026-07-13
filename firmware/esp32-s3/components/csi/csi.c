/**
 * csi.c — Wi-Fi CSI capture helper
 *
 * Wraps esp_wifi_set_csi_config / esp_wifi_set_csi_rx_cb so that
 * other firmware modules do not need to duplicate the CSI configuration.
 */

#include "csi.h"
#include "esp_log.h"
#include "esp_wifi.h"

static const char *TAG = "CSI";

void csi_init(wifi_csi_cb_t callback, void *ctx)
{
    wifi_csi_config_t cfg = {
        .lltf_en           = true,
        .htltf_en          = true,
        .stbc_htltf2_en    = true,
        .ltf_merge_en      = true,
        .channel_filter_en = true,
        .manu_scale        = false,
        .shift             = false,
    };
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(callback, ctx));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));
    ESP_LOGI(TAG, "CSI capture initialised");
}

void csi_set_enabled(bool enabled)
{
    ESP_ERROR_CHECK(esp_wifi_set_csi(enabled));
    ESP_LOGI(TAG, "CSI capture %s", enabled ? "enabled" : "disabled");
}
