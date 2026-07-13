#include <stdio.h>
#include "esp_log.h"

static const char *TAG = "HW_CTRL";

void display_show_alert(const char *severity, float freq) {
    ESP_LOGI(TAG, "Display alert severity=%s freq=%.1f", severity ? severity : "none", freq);
}

void vibrator_pulse(int duration_ms) {
    ESP_LOGI(TAG, "Vibrator pulse duration=%dms", duration_ms);
}

void led_flash_pattern(const char *pattern) {
    ESP_LOGI(TAG, "LED pattern=%s", pattern ? pattern : "default");
}

void wifi_transmit_noise(float freq_mhz, int duration_ms) {
    ESP_LOGI(TAG, "RF noise burst freq=%.1fMHz duration=%dms", freq_mhz, duration_ms);
}

void wifi_lock_channel(int channel) {
    ESP_LOGI(TAG, "Wi-Fi lock channel=%d", channel);
}

void sdcard_write_log_marker(const char *event, const char *json_details) {
    ESP_LOGI(TAG, "SD marker event=%s details=%s", event ? event : "unknown", json_details ? json_details : "{}");
}
