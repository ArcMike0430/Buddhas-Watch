#include <stdio.h>

#include "esp_log.h"

static const char *TAG = "HW_STUBS";

void display_show_alert(const char *severity, float freq) {
    ESP_LOGI(TAG, "display_show_alert severity=%s freq=%.1f", severity ? severity : "none", freq);
}

void vibrator_pulse(int duration_ms) {
    ESP_LOGI(TAG, "vibrator_pulse duration=%d", duration_ms);
}

void led_flash_pattern(const char *pattern) {
    ESP_LOGI(TAG, "led_flash_pattern pattern=%s", pattern ? pattern : "none");
}

void wifi_transmit_noise(float freq_mhz, int duration_ms) {
    ESP_LOGI(TAG, "wifi_transmit_noise freq=%.1f duration=%d", freq_mhz, duration_ms);
}

void wifi_lock_channel(int channel) {
    ESP_LOGI(TAG, "wifi_lock_channel ch=%d", channel);
}

void sdcard_write_log_marker(const char *event, const char *json_details) {
    ESP_LOGI(TAG, "sdcard_write_log_marker event=%s details=%s", event ? event : "unknown", json_details ? json_details : "{}");
}
