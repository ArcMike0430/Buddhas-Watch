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
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#define WIFI_SSID         "Buddhas-Net"
#define WIFI_PASS         "your_password"
#define JETSON_IP         "192.168.1.100"
#define UDP_PORT          5500
#define NODE_ID           "watch_left"

static const char *TAG = "CSI_WATCH";
static int udp_sock = -1;
static struct sockaddr_in dest_addr;
static EventGroupHandle_t wifi_event_group;

#define WIFI_CONNECTED_BIT BIT0

extern void start_cmd_receiver(void);

// Wi-Fi event handler
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Wi-Fi disconnected, retrying...");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// CSI callback — fires on each received packet
static void wifi_csi_cb(void *ctx, wifi_csi_info_t *data) {
    if (!data || !data->buf || udp_sock < 0) return;

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
        float phase = atan2f((float)data->buf[i].imag, (float)data->buf[i].real);
        offset += snprintf(packet + offset, sizeof(packet) - offset,
            "%s%.2f", i > 0 ? "," : "", phase);
    }

    snprintf(packet + offset, sizeof(packet) - offset, "]}");

    // Send UDP
    sendto(udp_sock, packet, strlen(packet), 0,
           (struct sockaddr *)&dest_addr, sizeof(dest_addr));
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

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();
    ESP_LOGI(TAG, "Wi-Fi connecting to %s...", WIFI_SSID);

    // Wait for IP address
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                        false, true, portMAX_DELAY);
    ESP_LOGI(TAG, "Wi-Fi connected");

    // Set up UDP socket
    udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(UDP_PORT);
    inet_aton(JETSON_IP, &dest_addr.sin_addr);
    ESP_LOGI(TAG, "UDP target: %s:%d", JETSON_IP, UDP_PORT);

    // Start command receiver
    start_cmd_receiver();

    // Enable CSI capture
    enable_csi();

    ESP_LOGI(TAG, "Buddhas-Watch CSI node ready");
}
