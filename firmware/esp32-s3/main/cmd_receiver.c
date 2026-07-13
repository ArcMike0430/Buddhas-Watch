/**
 * cmd_receiver.c — UDP command receiver for ESP32 watch
 *
 * Listens on port 5501 for commands from the Jetson monitor:
 *   - "alert":      Flash LED + vibrate + display warning
 *   - "rf_burst":   Transmit RF noise on specific band
 *   - "lock":       Lock Wi-Fi channel (efuse-style)
 *   - "log_marker": Write marker to SD card log
 *   - "sweep":      Sweep radio across frequency range
 *   - "silence":    Cancel all active counter-measures
 *   - "sync":       Sync system time to UTC epoch
 *   - "calibrate":  Start baseline calibration window
 *   - "mode":       Switch operating mode
 *   - "shutdown":   Enter deep sleep
 *
 * Compile with: idf.py build (part of main component)
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "cJSON.h"

#include "display/display.h"
#include "sdcard/sdcard.h"
#include "wifi_ctrl/wifi_ctrl.h"

#define CMD_PORT        5501
#define CMD_BUFFER_SIZE 1024

static const char *TAG = "CMD_RX";
static int cmd_sock = -1;

/**
 * Handle an incoming counter-measure command.
 */
static void handle_command(const char *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse command JSON: %.64s", json_str);
        return;
    }

    cJSON *cmd_item = cJSON_GetObjectItem(root, "cmd");
    cJSON *params   = cJSON_GetObjectItem(root, "params");

    if (!cmd_item || !cJSON_IsString(cmd_item)) {
        ESP_LOGW(TAG, "Missing or non-string 'cmd' field");
        cJSON_Delete(root);
        return;
    }

    const char *command = cmd_item->valuestring;

    /* Extract common params */
    float freq       = 0.0f;
    int duration_ms  = 100;
    cJSON *freq_item = params ? cJSON_GetObjectItem(params, "freq")        : NULL;
    cJSON *dur_item  = params ? cJSON_GetObjectItem(params, "duration_ms") : NULL;
    if (freq_item && cJSON_IsNumber(freq_item)) freq       = (float)freq_item->valuedouble;
    if (dur_item  && cJSON_IsNumber(dur_item))  duration_ms = dur_item->valueint;

    ESP_LOGI(TAG, "CMD: %s (freq=%.0f Hz, dur=%d ms)", command, freq, duration_ms);

    if (strcmp(command, "alert") == 0) {
        cJSON *sev_item = params ? cJSON_GetObjectItem(params, "severity") : NULL;
        const char *sev = (sev_item && cJSON_IsString(sev_item)) ? sev_item->valuestring : "medium";
        display_show_alert(sev, freq);
        wifi_ctrl_vibrate(duration_ms);
        display_led_flash(sev);
    }
    else if (strcmp(command, "rf_burst") == 0) {
        wifi_ctrl_transmit_noise(freq, duration_ms);
    }
    else if (strcmp(command, "lock") == 0) {
        cJSON *ch_item = params ? cJSON_GetObjectItem(params, "channel") : NULL;
        int channel = (ch_item && cJSON_IsNumber(ch_item)) ? ch_item->valueint : 0;
        wifi_ctrl_lock_channel(channel);
    }
    else if (strcmp(command, "log_marker") == 0) {
        cJSON *ev_item = params ? cJSON_GetObjectItem(params, "event") : NULL;
        const char *ev = (ev_item && cJSON_IsString(ev_item)) ? ev_item->valuestring : "unknown";
        char *details_str = params ? cJSON_PrintUnformatted(params) : NULL;
        sdcard_write_log_marker(ev, details_str ? details_str : "{}");
        if (details_str) free(details_str);
    }
    else if (strcmp(command, "sweep") == 0) {
        /* Sweep across Wi-Fi channels 1–13 covering 2.4 GHz band.
         * start_mhz / end_mhz map to 2412 MHz (ch 1) … 2472 MHz (ch 13).
         * Channel = (freq_mhz - 2412) / 5 + 1, clamped to [1, 13]. */
        /* 2.4 GHz channel mapping constants (shared with wifi_ctrl component):
         *   Channel 1 = 2412 MHz, channel spacing = 5 MHz
         *   Channel = (freq_mhz - WIFI_CH1_FREQ_MHZ) / WIFI_CHANNEL_SPACING_MHZ + 1 */
        #define WIFI_CH1_FREQ_MHZ        2412.0f
        #define WIFI_CHANNEL_SPACING_MHZ    5.0f

        cJSON *start_item = params ? cJSON_GetObjectItem(params, "start_mhz") : NULL;
        cJSON *end_item   = params ? cJSON_GetObjectItem(params, "end_mhz")   : NULL;
        float start_mhz   = (start_item && cJSON_IsNumber(start_item)) ? (float)start_item->valuedouble : 2412.0f;
        float end_mhz     = (end_item   && cJSON_IsNumber(end_item))   ? (float)end_item->valuedouble   : 2472.0f;

        int ch_start = (int)((start_mhz - WIFI_CH1_FREQ_MHZ) / WIFI_CHANNEL_SPACING_MHZ) + 1;
        int ch_end   = (int)((end_mhz   - WIFI_CH1_FREQ_MHZ) / WIFI_CHANNEL_SPACING_MHZ) + 1;
        if (ch_start < 1)  ch_start = 1;
        if (ch_end   > 13) ch_end   = 13;
        if (ch_start > ch_end) ch_start = ch_end;

        ESP_LOGI(TAG, "Sweep channels %d→%d (%.0f–%.0f MHz)", ch_start, ch_end, start_mhz, end_mhz);
        for (int ch = ch_start; ch <= ch_end; ch++) {
            wifi_ctrl_lock_channel(ch);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    else if (strcmp(command, "silence") == 0) {
        display_show_alert("none", 0.0f);
        wifi_ctrl_cancel_noise();
        ESP_LOGI(TAG, "All counter-measures silenced");
    }
    else if (strcmp(command, "sync") == 0) {
        /* Time sync — log received UTC epoch */
        cJSON *epoch_item = params ? cJSON_GetObjectItem(params, "utc_epoch_s") : NULL;
        if (epoch_item && cJSON_IsNumber(epoch_item)) {
            ESP_LOGI(TAG, "Time sync: %.0f s (UTC)", epoch_item->valuedouble);
            /* TODO: set RTC (PCF85063) via i2c_master_write if RTC component is available */
        }
    }
    else if (strcmp(command, "calibrate") == 0) {
        cJSON *dur_s = params ? cJSON_GetObjectItem(params, "duration_s") : NULL;
        int seconds  = (dur_s && cJSON_IsNumber(dur_s)) ? dur_s->valueint : 120;
        ESP_LOGI(TAG, "Baseline calibration window: %d s", seconds);
        sdcard_write_log_marker("calibrate_start", "{}");
        display_show_status("Calibrating...");
    }
    else if (strcmp(command, "mode") == 0) {
        cJSON *mode_item = params ? cJSON_GetObjectItem(params, "mode") : NULL;
        const char *mode = (mode_item && cJSON_IsString(mode_item)) ? mode_item->valuestring : "monitor";
        ESP_LOGI(TAG, "Mode → %s", mode);
        display_show_status(mode);
        char detail[64];
        snprintf(detail, sizeof(detail), "{\"mode\":\"%s\"}", mode);
        sdcard_write_log_marker("mode_change", detail);
    }
    else if (strcmp(command, "shutdown") == 0) {
        ESP_LOGI(TAG, "Entering deep sleep");
        sdcard_write_log_marker("shutdown", "{}");
        display_show_status("Sleeping...");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_deep_sleep_start();
    }
    else {
        ESP_LOGW(TAG, "Unknown command: %s", command);
    }

    cJSON_Delete(root);
}

/**
 * UDP command receiver task.
 * Runs continuously, blocking on recvfrom().
 */
static void cmd_receiver_task(void *pvParameters)
{
    cmd_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (cmd_sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(CMD_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(cmd_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind port %d", CMD_PORT);
        close(cmd_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Command receiver listening on port %d", CMD_PORT);

    char buffer[CMD_BUFFER_SIZE];
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    while (1) {
        int len = recvfrom(cmd_sock, buffer, sizeof(buffer) - 1, 0,
                           (struct sockaddr *)&client_addr, &client_len);
        if (len > 0) {
            buffer[len] = '\0';
            ESP_LOGD(TAG, "RX from %s: %.*s", inet_ntoa(client_addr.sin_addr), len, buffer);
            handle_command(buffer);
        }
    }
}

/**
 * Start the command receiver task.
 * Call from app_main() after Wi-Fi is connected.
 */
void start_cmd_receiver(void)
{
    xTaskCreate(cmd_receiver_task, "cmd_rx", 4096, NULL, 5, NULL);
}

