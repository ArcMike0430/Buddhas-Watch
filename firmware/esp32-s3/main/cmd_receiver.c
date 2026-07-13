/**
 * cmd_receiver.c — UDP command receiver for ESP32 watch
 *
 * Listens on port 5501 for commands from the Jetson monitor:
 *   - "alert":    Flash LED + vibrate + display warning
 *   - "rf_burst": Transmit RF noise on specific band
 *   - "lock":     Lock Wi-Fi channel (efuse-style)
 *   - "log_marker": Write marker to SD card log
 *   - "sweep":    Sweep radio across frequency range
 *   - "silence":  Cancel all active counter-measures
 *
 * Compile with: idf.py build (part of main component)
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "cJSON.h"
#include "cmd_receiver.h"

#define CMD_PORT        5501
#define CMD_BUFFER_SIZE 1024

static const char *TAG = "CMD_RX";
static int cmd_sock = -1;

// Forward declarations of hardware control functions
// (implemented elsewhere in your firmware)
extern void display_show_alert(const char *severity, float freq);
extern void vibrator_pulse(int duration_ms);
extern void led_flash_pattern(const char *pattern);
extern void wifi_transmit_noise(float freq_mhz, int duration_ms);
extern void wifi_lock_channel(int channel);
extern void sdcard_write_log_marker(const char *event, const char *json_details);

/**
 * Handle an incoming counter-measure command.
 */
static void handle_command(const char *json_str) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse command JSON");
        return;
    }

    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    cJSON *params = cJSON_GetObjectItem(root, "params");
    if (!cmd || !cJSON_IsString(cmd)) {
        cJSON_Delete(root);
        return;
    }

    const char *command = cJSON_IsString(cmd) ? cmd->valuestring : "";

    // Extract common params
    float freq = 0;
    int duration_ms = 100;
    cJSON *freq_item = params ? cJSON_GetObjectItem(params, "freq") : NULL;
    cJSON *dur_item = params ? cJSON_GetObjectItem(params, "duration_ms") : NULL;
    if (freq_item) freq = freq_item->valuedouble;
    if (dur_item) duration_ms = dur_item->valueint;

    ESP_LOGI(TAG, "CMD: %s (freq=%.0f, dur=%d)", command, freq, duration_ms);

    if (strcmp(command, "alert") == 0) {
        // Flash display + vibrate
        cJSON *severity = params ? cJSON_GetObjectItem(params, "severity") : NULL;
        const char *sev = severity && cJSON_IsString(severity) ? severity->valuestring : "medium";
        display_show_alert(sev, freq);
        vibrator_pulse(duration_ms);
        led_flash_pattern(sev);
    }
    else if (strcmp(command, "rf_burst") == 0) {
        // Transmit RF noise
        wifi_transmit_noise(freq, duration_ms);
    }
    else if (strcmp(command, "lock") == 0) {
        // Lock Wi-Fi channel
        cJSON *ch = params ? cJSON_GetObjectItem(params, "channel") : NULL;
        int channel = ch ? ch->valueint : 0;
        wifi_lock_channel(channel);
    }
    else if (strcmp(command, "log_marker") == 0) {
        // Write marker to SD card
        cJSON *event = params ? cJSON_GetObjectItem(params, "event") : NULL;
        const char *ev = event && cJSON_IsString(event) ? event->valuestring : "unknown";
        char *details_str = params ? cJSON_PrintUnformatted(params) : NULL;
        sdcard_write_log_marker(ev, details_str ? details_str : "{}");
        if (details_str) free(details_str);
    }
    else if (strcmp(command, "sweep") == 0) {
        // Frequency sweep
        cJSON *start = params ? cJSON_GetObjectItem(params, "start_mhz") : NULL;
        cJSON *end = params ? cJSON_GetObjectItem(params, "end_mhz") : NULL;
        float s = start ? start->valuedouble : 2400;
        float e = end ? end->valuedouble : 2500;
        // Implement sweep: step through channels
        for (int ch = (int)s; ch <= (int)e; ch++) {
            wifi_lock_channel(ch);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    else if (strcmp(command, "silence") == 0) {
        // Cancel all active counter-measures
        display_show_alert("none", 0);
        ESP_LOGI(TAG, "All counter-measures silenced");
    }

    cJSON_Delete(root);
}

/**
 * UDP command receiver task.
 * Runs continuously, blocking on recvfrom().
 */
static void cmd_receiver_task(void *pvParameters) {
    cmd_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (cmd_sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CMD_PORT),
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
            handle_command(buffer);
        }
    }
}

/**
 * Start the command receiver task.
 * Call from app_main() after Wi-Fi is connected.
 */
void start_cmd_receiver(void) {
    xTaskCreate(cmd_receiver_task, "cmd_rx", 4096, NULL, 5, NULL);
}
