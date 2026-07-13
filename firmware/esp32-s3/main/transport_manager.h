#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    TRANSPORT_NONE = 0,
    TRANSPORT_UDP,
    TRANSPORT_BLE,
    TRANSPORT_USB_CDC
} transport_type_t;

typedef struct {
    bool wifi_available;
    bool ble_available;
    bool usb_available;
    const char *jetson_ip;
    int udp_port;
} transport_config_t;

esp_err_t transport_manager_init(const transport_config_t *config);
transport_type_t transport_manager_active(void);
esp_err_t transport_manager_set_active(transport_type_t transport);
esp_err_t transport_send_csi(const char *payload, size_t len);
