/**
 * Buddhas-Watch ESP32-S3 CSI Firmware
 * Main entry point — Wi-Fi CSI capture + UDP streaming to Jetson/companion apps
 *
 * Captures CSI on all 52 subcarriers and streams phase/amplitude
 * data via UDP to Jetson Orin Nano and companion apps for real-time analysis.
 * Also launches settings, BLE CSI, and app manager tasks.
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

#include "settings_app.h"
#include "wifi_csi_app.h"
#include "ble_csi_app.h"
#include "app_manager.h"

#define WIFI_SSID           "Buddhas-Net"
#define WIFI_PASS           "your_password"
#define JETSON_IP           "192.168.1.100"
#define UDP_PORT            5500
#define NODE_ID             "watch_left"
#define WIFI_MAXIMUM_RETRY  5

/* Bit flags used in the event group */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static const char *TAG = "CSI_WATCH";
static int udp_sock = -1;
static struct sockaddr_in dest_addr;

/* Event group for Wi-Fi connection synchronisation */
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count = 0;

/* --------------------------------------------------------------------------
 * Wi-Fi event handler
 * -------------------------------------------------------------------------- */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "Retrying Wi-Fi connection (%d/%d)...",
                     s_retry_count, WIFI_MAXIMUM_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "Wi-Fi connection failed after %d retries", WIFI_MAXIMUM_RETRY);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* --------------------------------------------------------------------------
 * CSI callback — fires on each received packet
 * -------------------------------------------------------------------------- */
static void wifi_csi_cb(void *ctx, wifi_csi_info_t *data)
{
    if (!data || !data->buf || udp_sock < 0) return;

    /* Build JSON packet */
    char packet[2048];
    int offset = snprintf(packet, sizeof(packet),
        "{\"node_id\":\"%s\",\"timestamp\":%llu,\"rssi\":%d,\"rate\":%u,"
        "\"sig_mode\":%u,\"channel\":%u,\"secondary_channel\":%u,"
        "\"phases\":[",
        NODE_ID,
        (unsigned long long)esp_timer_get_time(),
        data->rx_ctrl.rssi,
        data->rx_ctrl.rate,
        data->rx_ctrl.sig_mode,
        data->rx_ctrl.primary_channel,
        data->rx_ctrl.secondary_channel);

    /* Append phase values (proper angle via atan2f) for all subcarriers */
    for (int i = 0; i < data->len && offset < 1900; i++) {
        float phase = atan2f((float)data->buf[i].imag, (float)data->buf[i].real);
        int written = snprintf(packet + offset,
                               (size_t)(sizeof(packet) - offset),
                               "%s%.4f", i > 0 ? "," : "", phase);
        if (written < 0 || written >= (int)(sizeof(packet) - offset)) break;
        offset += written;
    }

    /* Append magnitude array */
    {
        int written = snprintf(packet + offset,
                               (size_t)(sizeof(packet) - offset),
                               "],\"magnitudes\":[");
        if (written > 0 && written < (int)(sizeof(packet) - offset))
            offset += written;
    }
    for (int i = 0; i < data->len && offset < 1980; i++) {
        float mag = sqrtf((float)(data->buf[i].real * data->buf[i].real) +
                          (float)(data->buf[i].imag * data->buf[i].imag));
        int written = snprintf(packet + offset,
                               (size_t)(sizeof(packet) - offset),
                               "%s%.4f", i > 0 ? "," : "", mag);
        if (written < 0 || written >= (int)(sizeof(packet) - offset)) break;
        offset += written;
    }

    if (offset < (int)sizeof(packet) - 3) {
        snprintf(packet + offset, (size_t)(sizeof(packet) - offset), "]}");
    }
    /* Send UDP to Jetson/companion */
    sendto(udp_sock, packet, strlen(packet), 0,
           (struct sockaddr *)&dest_addr, sizeof(dest_addr));
}

/* --------------------------------------------------------------------------
 * Enable CSI capture
 * -------------------------------------------------------------------------- */
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
    ESP_LOGI(TAG, "CSI enabled");
}

/* --------------------------------------------------------------------------
 * app_main
 * -------------------------------------------------------------------------- */
void app_main(void)
{
    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Create event group before registering handlers */
    s_wifi_event_group = xEventGroupCreate();

    /* Initialize TCP/IP stack and event loop */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    /* Initialize Wi-Fi driver */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    /* Register Wi-Fi and IP event handlers */
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    /* Configure and start Wi-Fi */
    wifi_config_t wifi_config = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Wi-Fi connecting to %s...", WIFI_SSID);

    /* Block until connected or all retries exhausted */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Wi-Fi connected to %s", WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGW(TAG, "Wi-Fi connection failed — running in offline mode");
    }

    /* Unregister event handlers (no longer needed after connection attempt) */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                          instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                          instance_any_id));
    vEventGroupDelete(s_wifi_event_group);

    /* Set up UDP socket for CSI streaming */
    udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_sock >= 0) {
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port   = htons(UDP_PORT);
        inet_aton(JETSON_IP, &dest_addr.sin_addr);
        ESP_LOGI(TAG, "UDP target: %s:%d", JETSON_IP, UDP_PORT);
    } else {
        ESP_LOGW(TAG, "UDP socket creation failed — CSI streaming disabled");
    }

    /* Enable raw CSI capture (for Jetson UDP streaming) */
    enable_csi();

    /* Start command receiver for counter-measure commands */
    extern void start_cmd_receiver(void);
    start_cmd_receiver();

    /* Launch watch application tasks */
    settings_app_start();
    wifi_csi_app_start();
    ble_csi_app_start();
    app_manager_start();

    ESP_LOGI(TAG, "Buddhas-Watch CSI node ready — all tasks running");
}
