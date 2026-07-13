/**
 * Buddhas-Watch ESP32-S3 CSI Firmware
 * Main entry point — Wi-Fi CSI capture + UDP streaming to Jetson
 *
 * Captures CSI on all 52 subcarriers and streams phase/amplitude
 * data via UDP to the Jetson Orin Nano for real-time analysis.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "csi/csi.h"
#include "imu/imu.h"
#include "audio/audio.h"
#include "display/display.h"
#include "sdcard/sdcard.h"
#include "ble/ble.h"
#include "wifi_ctrl/wifi_ctrl.h"
#include "power/power.h"

/* ---- build-time configuration (override via idf.py menuconfig) ---- */
#ifndef CONFIG_WIFI_SSID
#define CONFIG_WIFI_SSID     "Buddhas-Net"
#endif
#ifndef CONFIG_WIFI_PASS
#define CONFIG_WIFI_PASS     "your_password"
#endif
#ifndef CONFIG_JETSON_IP
#define CONFIG_JETSON_IP     "192.168.1.100"
#endif
#ifndef CONFIG_UDP_PORT
#define CONFIG_UDP_PORT      5500
#endif
#ifndef CONFIG_NODE_ID
#define CONFIG_NODE_ID       "watch_left"
#endif

static const char *TAG = "CSI_WATCH";

/* Wi-Fi event group bits */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_MAX_RETRY      10

static EventGroupHandle_t wifi_event_group;
static int wifi_retry_count = 0;

static int udp_sock = -1;
static struct sockaddr_in dest_addr;

/* ------------------------------------------------------------------ */
/*  Wi-Fi event handler                                                 */
/* ------------------------------------------------------------------ */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (wifi_retry_count < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            wifi_retry_count++;
            ESP_LOGI(TAG, "Retrying Wi-Fi connection (%d/%d)...", wifi_retry_count, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "Wi-Fi connection failed after %d retries", WIFI_MAX_RETRY);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ------------------------------------------------------------------ */
/*  CSI callback — fires on each received Wi-Fi packet                  */
/* ------------------------------------------------------------------ */
static void wifi_csi_cb(void *ctx, wifi_csi_info_t *data)
{
    if (!data || !data->buf || udp_sock < 0) return;

    char packet[2048];
    int offset = snprintf(packet, sizeof(packet),
        "{\"node_id\":\"%s\",\"timestamp\":%llu,\"rssi\":%d,\"rate\":%u,"
        "\"sig_mode\":%u,\"channel\":%u,\"secondary_channel\":%u,"
        "\"phases\":[",
        CONFIG_NODE_ID,
        (unsigned long long)esp_timer_get_time(),
        data->rx_ctrl.rssi,
        data->rx_ctrl.rate,
        data->rx_ctrl.sig_mode,
        data->rx_ctrl.primary_channel,
        data->rx_ctrl.secondary_channel);

    /* Extract true phase angle via atan2f(imag, real) for each subcarrier */
    for (int i = 0; i < data->len && offset < (int)(sizeof(packet) - 16); i++) {
        float phase = atan2f((float)data->buf[i].imag, (float)data->buf[i].real);
        offset += snprintf(packet + offset, sizeof(packet) - offset,
            "%s%.4f", i > 0 ? "," : "", phase);
    }

    snprintf(packet + offset, sizeof(packet) - offset, "]}");

    sendto(udp_sock, packet, strlen(packet), 0,
           (struct sockaddr *)&dest_addr, sizeof(dest_addr));
}

/* ------------------------------------------------------------------ */
/*  Enable Wi-Fi CSI capture                                            */
/* ------------------------------------------------------------------ */
static void enable_csi(void)
{
    wifi_csi_config_t csi_config = {
        .lltf_en           = true,
        .htltf_en          = true,
        .stbc_htltf2_en    = true,
        .ltf_merge_en      = true,
        .channel_filter_en = true,
        .manu_scale        = false,
        .shift             = false,
    };
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi_config));
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(wifi_csi_cb, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));
    ESP_LOGI(TAG, "CSI capture enabled (%d subcarriers)", 52);
}

/* ------------------------------------------------------------------ */
/*  app_main                                                            */
/* ------------------------------------------------------------------ */
void app_main(void)
{
    /* Power management */
    power_init();

    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Peripheral init */
    sdcard_init();
    display_init();
    imu_init();
    audio_init();

    display_show_status("Booting...");

    /* Wi-Fi */
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t inst_wifi, inst_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &inst_wifi));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &inst_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid     = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    display_show_status("Connecting Wi-Fi...");
    ESP_LOGI(TAG, "Connecting to SSID: %s", CONFIG_WIFI_SSID);

    /* Block until connected or failed */
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Wi-Fi connected");
        display_show_status("Wi-Fi OK");
    } else {
        ESP_LOGE(TAG, "Wi-Fi failed — running in SD-card-only mode");
        display_show_alert("error", 0);
    }

    /* UDP transmit socket */
    udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_sock < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket");
    } else {
        dest_addr.sin_family      = AF_INET;
        dest_addr.sin_port        = htons(CONFIG_UDP_PORT);
        inet_aton(CONFIG_JETSON_IP, &dest_addr.sin_addr);
        ESP_LOGI(TAG, "UDP target: %s:%d", CONFIG_JETSON_IP, CONFIG_UDP_PORT);
    }

    /* BLE auxiliary channel */
    ble_init(CONFIG_NODE_ID);

    /* Start command receiver (port 5501) */
    extern void start_cmd_receiver(void);
    start_cmd_receiver();

    /* Enable CSI capture */
    enable_csi();

    sdcard_write_log_marker("boot", "{\"node\":\"" CONFIG_NODE_ID "\"}");
    display_show_status("Running");
    ESP_LOGI(TAG, "Buddhas-Watch node '%s' ready", CONFIG_NODE_ID);
}

