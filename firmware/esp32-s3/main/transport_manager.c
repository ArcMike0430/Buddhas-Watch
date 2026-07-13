#include <string.h>
#include "transport_manager.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "TRANSPORT";

static transport_type_t s_active_transport = TRANSPORT_NONE;
static int s_udp_sock = -1;
static struct sockaddr_in s_udp_addr = {0};

static esp_err_t init_udp(const transport_config_t *config) {
    s_udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_udp_sock < 0) {
        ESP_LOGE(TAG, "UDP socket creation failed");
        return ESP_FAIL;
    }

    s_udp_addr.sin_family = AF_INET;
    s_udp_addr.sin_port = htons(config->udp_port);
    inet_aton(config->jetson_ip, &s_udp_addr.sin_addr);
    return ESP_OK;
}

esp_err_t transport_manager_init(const transport_config_t *config) {
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (config->wifi_available) {
        if (init_udp(config) == ESP_OK) {
            s_active_transport = TRANSPORT_UDP;
            ESP_LOGI(TAG, "Selected UDP transport");
            return ESP_OK;
        }
    }

    if (config->ble_available) {
        s_active_transport = TRANSPORT_BLE;
        ESP_LOGW(TAG, "Selected BLE fallback transport");
        return ESP_OK;
    }

    if (config->usb_available) {
        s_active_transport = TRANSPORT_USB_CDC;
        ESP_LOGW(TAG, "Selected USB CDC fallback transport");
        return ESP_OK;
    }

    s_active_transport = TRANSPORT_NONE;
    return ESP_FAIL;
}

transport_type_t transport_manager_active(void) {
    return s_active_transport;
}

esp_err_t transport_manager_set_active(transport_type_t transport) {
    s_active_transport = transport;
    return ESP_OK;
}

esp_err_t transport_send_csi(const char *payload, size_t len) {
    if (!payload || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (s_active_transport) {
        case TRANSPORT_UDP:
            if (s_udp_sock < 0) {
                return ESP_FAIL;
            }
            if (sendto(s_udp_sock, payload, len, 0, (struct sockaddr *)&s_udp_addr, sizeof(s_udp_addr)) < 0) {
                ESP_LOGW(TAG, "UDP send failed; waiting for fallback");
                return ESP_FAIL;
            }
            return ESP_OK;
        case TRANSPORT_BLE:
            ESP_LOGI(TAG, "BLE transport selected (packet len=%u)", (unsigned)len);
            return ESP_OK;
        case TRANSPORT_USB_CDC:
            ESP_LOGI(TAG, "USB CDC transport selected (packet len=%u)", (unsigned)len);
            return ESP_OK;
        case TRANSPORT_NONE:
        default:
            return ESP_FAIL;
    }
}
