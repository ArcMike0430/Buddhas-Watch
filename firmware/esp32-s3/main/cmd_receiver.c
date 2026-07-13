/**
 * cmd_receiver.c — UDP command receiver for ESP32 watch
 *
 * Listens on port 5501 for commands from the Jetson monitor:
 *   - "alert":      Flash LED + vibrate + display warning
 *   - "rf_burst":   Transmit RF noise on specific band
 *   - "lock":       Lock Wi-Fi channel
 *   - "log_marker": Write marker to SD card log
 *   - "sweep":      Sweep radio across Wi-Fi channel range
 *   - "silence":    Cancel all active counter-measures
 *
 * Hardware control functions are implemented in this file.
 * Pin assignments assume the UeeKKoo ESP32-S3-Touch-AMOLED-2.06 board.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "cJSON.h"

#define CMD_PORT        5501
#define CMD_BUFFER_SIZE 1024

/* Board GPIO assignments — adjust to match actual board schematic */
#define GPIO_VIBRATOR   GPIO_NUM_45   /* Vibrator motor drive pin          */
#define GPIO_LED        GPIO_NUM_46   /* Status LED pin                    */
#define LEDC_CHANNEL    LEDC_CHANNEL_0
#define LEDC_TIMER      LEDC_TIMER_0
#define LEDC_FREQ_HZ    5000

static const char *TAG       = "CMD_RX";
static int         cmd_sock  = -1;
static bool        hw_inited = false;

/* --------------------------------------------------------------------------
 * Hardware initialisation (called once before first use)
 * -------------------------------------------------------------------------- */
static void hw_init(void)
{
    if (hw_inited) return;

    /* Configure vibrator GPIO as output */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << GPIO_VIBRATOR) | (1ULL << GPIO_LED),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(GPIO_VIBRATOR, 0);
    gpio_set_level(GPIO_LED, 0);

    /* Configure LEDC timer for PWM-based LED brightness */
    ledc_timer_config_t ledc_timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz         = LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .channel    = LEDC_CHANNEL,
        .duty       = 0,
        .gpio_num   = GPIO_LED,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .hpoint     = 0,
        .timer_sel  = LEDC_TIMER,
    };
    ledc_channel_config(&ledc_channel);

    hw_inited = true;
    ESP_LOGI(TAG, "Hardware control pins initialised");
}

/* --------------------------------------------------------------------------
 * Hardware control function implementations
 * -------------------------------------------------------------------------- */

/**
 * display_show_alert — Show a visual alert on the AMOLED screen.
 * @param severity  "low", "medium", "high", or "none"
 * @param freq      Frequency in MHz (0 = none)
 *
 * A full LVGL implementation lives in settings_app.c / wifi_csi_app.c.
 * This function logs the alert and flashes the LED as a fallback.
 */
void display_show_alert(const char *severity, float freq)
{
    hw_init();
    if (strcmp(severity, "none") == 0) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL);
        ESP_LOGI(TAG, "Alert cleared");
        return;
    }

    uint32_t duty = 128; /* default: medium brightness */
    if (strcmp(severity, "high") == 0)        duty = 255;
    else if (strcmp(severity, "medium") == 0) duty = 160;
    else if (strcmp(severity, "low") == 0)    duty = 80;

    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL);
    ESP_LOGW(TAG, "ALERT [%s] freq=%.0f MHz — LED duty=%lu", severity, freq, (unsigned long)duty);
}

/**
 * vibrator_pulse — Drive the vibrator motor for the specified duration.
 * @param duration_ms  Duration in milliseconds (capped at 5000 ms for safety)
 */
void vibrator_pulse(int duration_ms)
{
    hw_init();
    if (duration_ms <= 0) return;
    if (duration_ms > 5000) duration_ms = 5000;

    gpio_set_level(GPIO_VIBRATOR, 1);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    gpio_set_level(GPIO_VIBRATOR, 0);
    ESP_LOGI(TAG, "Vibrator pulse %d ms done", duration_ms);
}

/**
 * led_flash_pattern — Flash the status LED with a predefined pattern.
 * @param pattern  "low" (1 pulse), "medium" (2 pulses), "high" (3 fast pulses)
 */
void led_flash_pattern(const char *pattern)
{
    hw_init();

    int pulses   = 1;
    int on_ms    = 200;
    int off_ms   = 200;

    if (strcmp(pattern, "high") == 0) {
        pulses = 3; on_ms = 100; off_ms = 100;
    } else if (strcmp(pattern, "medium") == 0) {
        pulses = 2; on_ms = 150; off_ms = 150;
    } else if (strcmp(pattern, "none") == 0) {
        gpio_set_level(GPIO_LED, 0);
        return;
    }

    for (int p = 0; p < pulses; p++) {
        gpio_set_level(GPIO_LED, 1);
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        gpio_set_level(GPIO_LED, 0);
        vTaskDelay(pdMS_TO_TICKS(off_ms));
    }
    ESP_LOGI(TAG, "LED flash pattern '%s' complete", pattern);
}

/**
 * wifi_transmit_noise — Transmit beacon/probe frames to generate RF noise.
 * Uses esp_wifi_80211_tx() to send a raw frame on the specified channel.
 * @param freq_mhz   Centre frequency in MHz (2400–2500 range)
 * @param duration_ms How long to transmit (ms)
 */
void wifi_transmit_noise(float freq_mhz, int duration_ms)
{
    /* Map MHz to Wi-Fi channel (1–13) */
    int channel = (int)roundf((freq_mhz - 2407.0f) / 5.0f);
    if (channel < 1)  channel = 1;
    if (channel > 13) channel = 13;

    /* Switch channel */
    uint8_t second = 0;
    esp_wifi_get_channel((uint8_t *)&channel, &second);
    esp_wifi_set_channel((uint8_t)channel, WIFI_SECOND_CHAN_NONE);

    /* Minimal 802.11 probe-request frame (broadcast) */
    static const uint8_t probe_frame[] = {
        0x40, 0x00,              /* Frame Control: probe request             */
        0x00, 0x00,              /* Duration                                 */
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, /* RA: broadcast                */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x01, /* TA: dummy source              */
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, /* BSSID: broadcast              */
        0x00, 0x00,              /* Sequence control                         */
        0x00,                    /* SSID parameter set (empty SSID)          */
        0x00,
        0x01, 0x08,              /* Supported rates IE                       */
        0x82, 0x84, 0x8B, 0x96, 0x24, 0x30, 0x48, 0x6C,
    };

    int64_t deadline = esp_timer_get_time() + (int64_t)duration_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        esp_wifi_80211_tx(WIFI_IF_STA, probe_frame, sizeof(probe_frame), false);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    ESP_LOGI(TAG, "RF noise burst complete on channel %d (%.0f MHz) for %d ms",
             channel, freq_mhz, duration_ms);
}

/**
 * wifi_lock_channel — Set the Wi-Fi radio to a fixed channel.
 * @param channel  Wi-Fi channel number (1–13)
 */
void wifi_lock_channel(int channel)
{
    if (channel < 1)  channel = 1;
    if (channel > 13) channel = 13;

    esp_err_t err = esp_wifi_set_channel((uint8_t)channel, WIFI_SECOND_CHAN_NONE);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi locked to channel %d", channel);
    } else {
        ESP_LOGW(TAG, "Failed to lock channel %d: %s", channel, esp_err_to_name(err));
    }
}

/**
 * sdcard_write_log_marker — Append a JSON marker to the SD card log file.
 * @param event        Short event label (e.g. "alert", "sweep")
 * @param json_details Serialised JSON params string
 *
 * Full SD card FATFS initialisation is handled by the data logger component.
 * This stub writes to UART so the marker is captured even if SD is absent.
 */
void sdcard_write_log_marker(const char *event, const char *json_details)
{
    int64_t ts = esp_timer_get_time();
    ESP_LOGI(TAG, "LOG_MARKER ts=%lld event=%s details=%s",
             (long long)ts, event, json_details);

    /* TODO: when SD card FATFS is mounted, open "/sdcard/log.jsonl" and append:
     *   {"ts":<us>,"event":"<event>","params":<json_details>}
     * This is handled by the data logging task in wifi_csi_app.c when the
     * SD card is initialised during startup.
     */
}

/* --------------------------------------------------------------------------
 * Command dispatch
 * -------------------------------------------------------------------------- */

/**
 * handle_command — Parse and execute a JSON command string.
 */
static void handle_command(const char *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse command JSON");
        return;
    }

    cJSON *cmd    = cJSON_GetObjectItem(root, "cmd");
    cJSON *params = cJSON_GetObjectItem(root, "params");

    if (!cmd || !cJSON_IsString(cmd)) {
        cJSON_Delete(root);
        return;
    }

    /* Fixed: use cmd->valuestring directly — cJSON_IsString is a function,
     * not a method.  The type has already been validated above. */
    const char *command = cmd->valuestring ? cmd->valuestring : "";

    /* Extract common params */
    float freq        = 0.0f;
    int   duration_ms = 100;
    cJSON *freq_item  = params ? cJSON_GetObjectItem(params, "freq") : NULL;
    cJSON *dur_item   = params ? cJSON_GetObjectItem(params, "duration_ms") : NULL;
    if (freq_item) freq        = (float)freq_item->valuedouble;
    if (dur_item)  duration_ms = dur_item->valueint;

    ESP_LOGI(TAG, "CMD: %s (freq=%.0f, dur=%d)", command, freq, duration_ms);

    if (strcmp(command, "alert") == 0) {
        cJSON      *severity = params ? cJSON_GetObjectItem(params, "severity") : NULL;
        const char *sev = severity && cJSON_IsString(severity)
                          ? severity->valuestring : "medium";
        display_show_alert(sev, freq);
        vibrator_pulse(duration_ms);
        led_flash_pattern(sev);
    }
    else if (strcmp(command, "rf_burst") == 0) {
        wifi_transmit_noise(freq, duration_ms);
    }
    else if (strcmp(command, "lock") == 0) {
        cJSON *ch      = params ? cJSON_GetObjectItem(params, "channel") : NULL;
        int    channel = ch ? ch->valueint : 6;
        wifi_lock_channel(channel);
    }
    else if (strcmp(command, "log_marker") == 0) {
        cJSON      *event = params ? cJSON_GetObjectItem(params, "event") : NULL;
        const char *ev    = event && cJSON_IsString(event)
                            ? event->valuestring : "unknown";
        char *details_str = params ? cJSON_PrintUnformatted(params) : NULL;
        sdcard_write_log_marker(ev, details_str ? details_str : "{}");
        if (details_str) free(details_str);
    }
    else if (strcmp(command, "sweep") == 0) {
        /* Fixed: convert start/end MHz to Wi-Fi channels (1–13).
         * 2412 MHz = ch 1, 2417 = ch 2, …, 2472 = ch 13
         * Formula: channel = round((freq_mhz - 2407) / 5) */
        cJSON *start = params ? cJSON_GetObjectItem(params, "start_mhz") : NULL;
        cJSON *end   = params ? cJSON_GetObjectItem(params, "end_mhz")   : NULL;
        float  s     = start ? (float)start->valuedouble : 2412.0f;
        float  e     = end   ? (float)end->valuedouble   : 2472.0f;

        int start_ch = (int)roundf((s - 2407.0f) / 5.0f);
        int end_ch   = (int)roundf((e - 2407.0f) / 5.0f);
        if (start_ch < 1)  start_ch = 1;
        if (start_ch > 13) start_ch = 13;
        if (end_ch   < 1)  end_ch   = 1;
        if (end_ch   > 13) end_ch   = 13;

        ESP_LOGI(TAG, "Sweep ch %d–%d (%.0f–%.0f MHz)", start_ch, end_ch, s, e);
        for (int ch = start_ch; ch <= end_ch; ch++) {
            wifi_lock_channel(ch);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    else if (strcmp(command, "silence") == 0) {
        display_show_alert("none", 0.0f);
        gpio_set_level(GPIO_VIBRATOR, 0);
        gpio_set_level(GPIO_LED, 0);
        ESP_LOGI(TAG, "All counter-measures silenced");
    }
    else {
        ESP_LOGW(TAG, "Unknown command: %s", command);
    }

    cJSON_Delete(root);
}

/* --------------------------------------------------------------------------
 * UDP command receiver task
 * -------------------------------------------------------------------------- */

/**
 * cmd_receiver_task — Runs continuously, blocking on recvfrom().
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

    char              buffer[CMD_BUFFER_SIZE];
    struct sockaddr_in client_addr;
    socklen_t          client_len = sizeof(client_addr);

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
 * start_cmd_receiver — Start the command receiver task.
 * Call from app_main() after Wi-Fi is connected.
 */
void start_cmd_receiver(void)
{
    xTaskCreate(cmd_receiver_task, "cmd_rx", 4096, NULL, 5, NULL);
}
