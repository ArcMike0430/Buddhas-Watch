/**
 * ble.c — BLE auxiliary channel (NimBLE / Bluedroid wrapper)
 *
 * Exposes a custom GATT service with one notify characteristic so
 * that a companion phone app can receive detection alerts.
 */

#include "ble.h"
#include <string.h>
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_main.h"
#include "nvs_flash.h"

/* Custom 128-bit service UUID: buddhas-watch-svc */
#define BUDDHAS_SVC_UUID128  \
    0xBD, 0xDA, 0x00, 0x01, 0x00, 0x00, 0x10, 0x00, \
    0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB

#define GATTS_APP_ID        0
#define CHAR_HANDLE_MAX     4

static const char *TAG = "BLE";
static uint16_t gatts_if_cached    = 0;
static uint16_t conn_id_cached     = 0xFFFF;
static uint16_t notify_char_handle = 0;
static bool     peer_connected     = false;
static char     device_name[32]    = "buddhas-watch";

/* ---- advertising data ---- */
static esp_ble_adv_data_t adv_data = {
    .set_scan_rsp        = false,
    .include_name        = true,
    .include_txpower     = true,
    .min_interval        = 0x0006,
    .max_interval        = 0x0010,
    .appearance          = 0x00C1,  /* Generic Watch */
    .manufacturer_len    = 0,
    .p_manufacturer_data = NULL,
    .service_data_len    = 0,
    .p_service_data      = NULL,
    .service_uuid_len    = 0,
    .p_service_uuid      = NULL,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_params_t adv_params = {
    .adv_int_min       = 0x0020,
    .adv_int_max       = 0x0040,
    .adv_type          = ADV_TYPE_IND,
    .own_addr_type     = BLE_ADDR_TYPE_PUBLIC,
    .channel_map       = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

/* ---- GATTS callback ---- */
static void gatts_event_handler(esp_gatts_cb_event_t event,
                                 esp_gatt_if_t gatts_if,
                                 esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT:
        gatts_if_cached = gatts_if;
        esp_ble_gap_set_device_name(device_name);
        esp_ble_gap_config_adv_data(&adv_data);
        break;

    case ESP_GATTS_CONNECT_EVT:
        conn_id_cached = param->connect.conn_id;
        peer_connected = true;
        ESP_LOGI(TAG, "BLE peer connected");
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        peer_connected = false;
        ESP_LOGI(TAG, "BLE peer disconnected — restarting adv");
        esp_ble_gap_start_advertising(&adv_params);
        break;

    case ESP_GATTS_READ_EVT:
        /* Respond with a short status string */
        esp_gatt_rsp_t rsp = {0};
        rsp.attr_value.len = snprintf((char *)rsp.attr_value.value,
                                       sizeof(rsp.attr_value.value), "OK");
        esp_ble_gatts_send_response(gatts_if, param->read.conn_id,
                                    param->read.trans_id, ESP_GATT_OK, &rsp);
        break;

    default:
        break;
    }
}

/* ---- GAP callback ---- */
static void gap_event_handler(esp_gap_ble_cb_event_t event,
                               esp_ble_gap_cb_param_t *param)
{
    if (event == ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT) {
        esp_ble_gap_start_advertising(&adv_params);
        ESP_LOGI(TAG, "BLE advertising started as '%s'", device_name);
    }
}

/* ------------------------------------------------------------------ */

void ble_init(const char *node_id)
{
    if (node_id && strlen(node_id) < sizeof(device_name)) {
        strncpy(device_name, node_id, sizeof(device_name) - 1);
    }

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_event_handler));
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(GATTS_APP_ID));

    ESP_LOGI(TAG, "BLE initialised (node_id='%s')", device_name);
}

void ble_notify(const char *message)
{
    if (!peer_connected || notify_char_handle == 0) return;

    size_t len = strlen(message);
    if (len > 20) len = 20;  /* ATT MTU minimum is 23, payload 20 */

    esp_ble_gatts_send_indicate(gatts_if_cached, conn_id_cached,
                                 notify_char_handle,
                                 (uint16_t)len, (uint8_t *)message, false);
}

void ble_deinit(void)
{
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    ESP_LOGI(TAG, "BLE stopped");
}
