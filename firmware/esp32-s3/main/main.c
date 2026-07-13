/**
 * Buddhas-Watch ESP32-S3 CSI Firmware
 * Main entry point — Wi-Fi CSI capture + UDP streaming to Jetson
 *
 * Captures CSI on all 52 subcarriers and streams phase/amplitude
 * data via UDP to the Jetson Orin Nano for real-time analysis.
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "config.h"
#include "cmd_receiver.h"
#include "transport_manager.h"
#include "lwip/err.h"

#define UDP_PORT          5500
#define NODE_ID           "watch_left"
#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "CSI_WATCH";
static EventGroupHandle_t wifi_event_group;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    (void)arg;
    (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// CSI callback — fires on each received packet
static void wifi_csi_cb(void *ctx, wifi_csi_info_t *data) {
    if (!data || !data->buf) return;

    // Build JSON packet
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

    // Append phase values for all subcarriers
    for (int i = 0; i < data->len && offset < 2000; i++) {
        float phase = data->buf[i].real + data->buf[i].imag * 1.0f;
        offset += snprintf(packet + offset, sizeof(packet) - offset,
            "%s%.2f", i > 0 ? "," : "", phase);
    }

    snprintf(packet + offset, sizeof(packet) - offset, "]}");

    (void)transport_send_csi(packet, strlen(packet));
}

// Enable CSI capture
static void enable_csi(void) {
    wifi_csi_config_t csi_config = {
        .lltf_en = true,
        .htltf_en = true,
        .stbc_htltf2_en = true,
        .ltf_merge_en = true,
        .channel_filter_en = true,
        .manu_scale = false,
        .shift = false,
    };
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi_config));
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(wifi_csi_cb, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));
    ESP_LOGI(TAG, "CSI enabled");
}

void app_main(void) {
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize Wi-Fi in station mode
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_event_group = xEventGroupCreate();

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = DEFAULT_WIFI_SSID,
            .password = DEFAULT_WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Wi-Fi connecting to %s...", DEFAULT_WIFI_SSID);

    // Wait for connection
    EventBits_t wifi_bits = xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(8000)
    );

    bool wifi_available = (wifi_bits & WIFI_CONNECTED_BIT) != 0;
#ifdef CONFIG_BT_ENABLED
    bool ble_available = true;
#else
    bool ble_available = false;
#endif
#ifdef CONFIG_TINYUSB_CDC_ENABLED
    bool usb_available = true;
#else
    bool usb_available = false;
#endif

    transport_config_t transport_config = {
        .wifi_available = wifi_available,
        .ble_available = ble_available,
        .usb_available = usb_available,
        .jetson_ip = DEFAULT_JETSON_IP,
        .udp_port = UDP_PORT
    };
    ESP_ERROR_CHECK(transport_manager_init(&transport_config));

    // Enable CSI capture
    enable_csi();
    start_cmd_receiver();

    ESP_LOGI(TAG, "Buddhas-Watch CSI node ready");
}
