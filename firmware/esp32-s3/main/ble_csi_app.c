/**
 * ble_csi_app.c — BLE GATT service for real-time CSI streaming
 *
 * Implements a custom BLE GATT service with three characteristics:
 *
 *   Service UUID:    0xAB00  (16-bit short UUID for simplicity;
 *                             map to 128-bit: 0000AB00-0000-1000-8000-00805F9B34FB)
 *
 *   Characteristics:
 *     0xAB01 — CSI Metadata  (notify, 20 bytes)
 *              Format: [channel:1][rssi:1][rate:2][timestamp:8][subcarrier_count:1][pad:7]
 *
 *     0xAB02 — CSI Data      (notify, 244 bytes max per packet, chunked)
 *              Format: [chunk_index:1][total_chunks:1][magnitudes/phases:...]
 *
 *     0xAB03 — Control       (write, 4 bytes)
 *              Values: 0x01=Start capture, 0x00=Stop capture, 0x02=Toggle SD logging
 *
 * Companions connect, subscribe to notifications on 0xAB01 and 0xAB02, and
 * receive continuous CSI data.  The control characteristic allows
 * start/stop/logging toggle without a Wi-Fi connection.
 *
 * BLE MTU is negotiated to 247 bytes (maximum for BLE 4.2+) for efficient
 * data transfer.
 *
 * Advertising:
 *   - Device name:   "BuddhasWatch"
 *   - Service UUID:  0xAB00
 *   - Connectable, undirected
 */

#include "ble_csi_app.h"
#include "settings_app.h"

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"
#include "esp_wifi.h"

/* LVGL — conditionally compiled */
#if defined(LV_VERSION_MAJOR)
#  include "lvgl.h"
#else
typedef void lv_obj_t;
#  define lv_label_create(p)              NULL
#  define lv_label_set_text_fmt(o,...)    (void)(o)
#  define lv_obj_align(o,a,x,y)          (void)(o)
#  define lv_btn_create(p)                NULL
#  define lv_obj_set_size(o,w,h)          (void)(o)
#  define lv_obj_add_event_cb(o,c,e,d)   (void)(o)
#  define lv_scr_act()                    NULL
#  define LV_EVENT_CLICKED                0
#  define LV_ALIGN_TOP_MID                0
#  define LV_ALIGN_TOP_LEFT               1
#  define LV_ALIGN_BOTTOM_RIGHT           2
#  define LV_ALIGN_CENTER                 3
#  define lv_task_handler()               do {} while(0)
#endif

#define TAG             "BLE_CSI"
#define DEVICE_NAME     "BuddhasWatch"
#define MAX_SUBCARRIERS 52

/* Attribute handles */
#define GATTS_NUM_HANDLES   10

/* UUIDs */
#define SERVICE_UUID        0xAB00
#define CHAR_METADATA_UUID  0xAB01
#define CHAR_DATA_UUID      0xAB02
#define CHAR_CTRL_UUID      0xAB03

/* Application profile */
#define GATTS_APP_ID        0

static const char *s_device_name = DEVICE_NAME;

typedef struct {
    esp_gatt_if_t  gatts_if;
    uint16_t       service_handle;
    uint16_t       meta_char_handle;
    uint16_t       meta_cccd_handle;
    uint16_t       data_char_handle;
    uint16_t       data_cccd_handle;
    uint16_t       ctrl_char_handle;
    uint16_t       conn_id;
    bool           connected;
    bool           meta_notify;
    bool           data_notify;
} ble_profile_t;

static ble_profile_t s_profile = {
    .gatts_if  = ESP_GATT_IF_NONE,
    .connected = false,
};

static bool s_capturing = false;

/* Latest CSI snapshot (shared with wifi_csi_app via extern; updated in CSI cb) */
extern volatile float s_mag[];
extern volatile float s_phase[];
extern volatile int   s_last_rssi;
extern volatile int   s_last_channel;
extern volatile uint32_t s_last_rate;

/* Advertising data */
static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp        = false,
    .include_name        = true,
    .include_txpower     = true,
    .min_interval        = 0x0006,
    .max_interval        = 0x0010,
    .appearance          = 0x00,
    .manufacturer_len    = 0,
    .p_manufacturer_data = NULL,
    .service_data_len    = 0,
    .p_service_data      = NULL,
    .service_uuid_len    = 0,
    .p_service_uuid      = NULL,
    .flag                = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min        = 0x20,
    .adv_int_max        = 0x40,
    .adv_type           = ADV_TYPE_IND,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .channel_map        = ADV_CHNL_ALL,
    .adv_filter_policy  = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

/* -------------------------------------------------------------------------
 * Helper: send CSI notification to connected client
 * ------------------------------------------------------------------------- */
static void ble_send_metadata(void)
{
    if (!s_profile.connected || !s_profile.meta_notify) return;

    uint8_t buf[20];
    memset(buf, 0, sizeof(buf));
    buf[0]  = (uint8_t)s_last_channel;
    buf[1]  = (uint8_t)((int8_t)s_last_rssi);  /* signed cast */
    buf[2]  = (uint8_t)(s_last_rate & 0xFF);
    buf[3]  = (uint8_t)(s_last_rate >> 8);
    int64_t ts = esp_timer_get_time();
    memcpy(&buf[4], &ts, 8);
    buf[12] = MAX_SUBCARRIERS;

    esp_ble_gatts_send_indicate(s_profile.gatts_if, s_profile.conn_id,
                                s_profile.meta_char_handle,
                                sizeof(buf), buf, false);
}

static void ble_send_csi_data(void)
{
    if (!s_profile.connected || !s_profile.data_notify) return;

    /* Pack magnitudes as uint8 (0–255) for compact BLE transfer */
    uint8_t  chunk[244];
    uint8_t  total_chunks = (uint8_t)((MAX_SUBCARRIERS + 59) / 60); /* ~60 values/chunk */

    for (uint8_t c = 0; c < total_chunks; c++) {
        int start = c * 60;
        int end   = start + 60;
        if (end > MAX_SUBCARRIERS) end = MAX_SUBCARRIERS;
        int  n_vals = end - start;

        chunk[0] = c;
        chunk[1] = total_chunks;
        for (int i = 0; i < n_vals; i++) {
            /* Magnitude: scale 0–255 assuming max ~64 */
            float m = s_mag[start + i];
            uint8_t mv = (uint8_t)(m < 0 ? 0 : (m > 255 ? 255 : m));
            /* Phase: map -π..+π → 0..255 */
            float ph = s_phase[start + i];
            uint8_t pv = (uint8_t)((ph + 3.14159f) / (2.0f * 3.14159f) * 255.0f);
            chunk[2 + i * 2]     = mv;
            chunk[2 + i * 2 + 1] = pv;
        }
        uint16_t len = (uint16_t)(2 + n_vals * 2);
        esp_ble_gatts_send_indicate(s_profile.gatts_if, s_profile.conn_id,
                                    s_profile.data_char_handle,
                                    len, chunk, false);
        vTaskDelay(pdMS_TO_TICKS(5)); /* small gap between chunks */
    }
}

/* -------------------------------------------------------------------------
 * GATTS event handler
 * ------------------------------------------------------------------------- */
static void gatts_event_handler(esp_gatts_cb_event_t event,
                                 esp_gatt_if_t gatts_if,
                                 esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT:
        ESP_LOGI(TAG, "GATTS registered, app_id %d", param->reg.app_id);
        s_profile.gatts_if = gatts_if;
        esp_ble_gap_set_device_name(s_device_name);
        esp_ble_gap_config_adv_data(&s_adv_data);

        /* Create primary service */
        esp_gatt_srvc_id_t srvc_id = {
            .is_primary = true,
            .id = {
                .inst_id = 0,
                .uuid = { .len = ESP_UUID_LEN_16, .uuid.uuid16 = SERVICE_UUID },
            },
        };
        esp_ble_gatts_create_service(gatts_if, &srvc_id, GATTS_NUM_HANDLES);
        break;

    case ESP_GATTS_CREATE_EVT: {
        s_profile.service_handle = param->create.service_handle;
        esp_ble_gatts_start_service(s_profile.service_handle);

        /* Add Metadata characteristic (notify) */
        esp_bt_uuid_t meta_uuid = {
            .len = ESP_UUID_LEN_16, .uuid.uuid16 = CHAR_METADATA_UUID
        };
        esp_ble_gatts_add_char(s_profile.service_handle, &meta_uuid,
                               ESP_GATT_PERM_READ,
                               ESP_GATT_CHAR_PROP_BIT_NOTIFY,
                               NULL, NULL);
        break;
    }

    case ESP_GATTS_ADD_CHAR_EVT:
        if (param->add_char.char_uuid.uuid.uuid16 == CHAR_METADATA_UUID) {
            s_profile.meta_char_handle = param->add_char.attr_handle;
            /* Add CSI Data characteristic */
            esp_bt_uuid_t data_uuid = {
                .len = ESP_UUID_LEN_16, .uuid.uuid16 = CHAR_DATA_UUID
            };
            esp_ble_gatts_add_char(s_profile.service_handle, &data_uuid,
                                   ESP_GATT_PERM_READ,
                                   ESP_GATT_CHAR_PROP_BIT_NOTIFY,
                                   NULL, NULL);
        } else if (param->add_char.char_uuid.uuid.uuid16 == CHAR_DATA_UUID) {
            s_profile.data_char_handle = param->add_char.attr_handle;
            /* Add Control characteristic (writable) */
            esp_bt_uuid_t ctrl_uuid = {
                .len = ESP_UUID_LEN_16, .uuid.uuid16 = CHAR_CTRL_UUID
            };
            esp_gatt_attr_val_t ctrl_val = {
                .attr_max_len = 4,
                .attr_len     = 1,
                .attr_value   = (uint8_t[]){0x00},
            };
            esp_ble_gatts_add_char(s_profile.service_handle, &ctrl_uuid,
                                   ESP_GATT_PERM_WRITE,
                                   ESP_GATT_CHAR_PROP_BIT_WRITE,
                                   &ctrl_val, NULL);
        } else {
            s_profile.ctrl_char_handle = param->add_char.attr_handle;
            ESP_LOGI(TAG, "All characteristics added — service ready");
        }
        break;

    case ESP_GATTS_CONNECT_EVT:
        s_profile.connected = true;
        s_profile.conn_id   = param->connect.conn_id;
        ESP_LOGI(TAG, "BLE client connected, conn_id=%d", param->connect.conn_id);
        esp_ble_conn_update_params_t conn_params = {
            .min_int  = 0x06, .max_int = 0x0C,
            .latency  = 0,    .timeout = 400,
        };
        memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        esp_ble_gap_update_conn_params(&conn_params);
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        s_profile.connected   = false;
        s_profile.meta_notify = false;
        s_profile.data_notify = false;
        ESP_LOGI(TAG, "BLE client disconnected — restarting advertising");
        esp_ble_gap_start_advertising(&s_adv_params);
        break;

    case ESP_GATTS_WRITE_EVT: {
        if (param->write.handle == s_profile.ctrl_char_handle && param->write.len >= 1) {
            uint8_t cmd = param->write.value[0];
            if (cmd == 0x01) {
                s_capturing = true;
                ESP_LOGI(TAG, "BLE: CSI capture started");
            } else if (cmd == 0x00) {
                s_capturing = false;
                ESP_LOGI(TAG, "BLE: CSI capture stopped");
            } else if (cmd == 0x02) {
                ESP_LOGI(TAG, "BLE: SD logging toggle");
            }
        }
        /* Handle CCCD writes (enable/disable notifications) */
        if (param->write.handle == s_profile.meta_char_handle + 1) {
            s_profile.meta_notify = (param->write.value[0] & 0x01) != 0;
        }
        if (param->write.handle == s_profile.data_char_handle + 1) {
            s_profile.data_notify = (param->write.value[0] & 0x01) != 0;
        }
        break;
    }

    case ESP_GATTS_MTU_EVT:
        ESP_LOGI(TAG, "BLE MTU set to %d bytes", param->mtu.mtu);
        break;

    default:
        break;
    }
}

/* -------------------------------------------------------------------------
 * GAP event handler
 * ------------------------------------------------------------------------- */
static void gap_event_handler(esp_gap_ble_cb_event_t event,
                               esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        esp_ble_gap_start_advertising(&s_adv_params);
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "BLE advertising started as '%s'", s_device_name);
        }
        break;
    default:
        break;
    }
}

/* -------------------------------------------------------------------------
 * BLE CSI task — sends notifications at ~10 Hz
 * ------------------------------------------------------------------------- */
static void ble_csi_task(void *pvParameters)
{
    ESP_LOGI(TAG, "BLE CSI task started");

#ifdef LV_VERSION_MAJOR
    /* Build a minimal BLE status screen */
    lv_obj_t *scr  = lv_scr_act();
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text_fmt(title, "BLE: %s", s_device_name);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *status_lbl = lv_label_create(scr);
    lv_label_set_text_fmt(status_lbl, "Status: Advertising...");
    lv_obj_align(status_lbl, LV_ALIGN_TOP_LEFT, 5, 30);
#endif

    while (1) {
        if (s_profile.connected && s_capturing) {
            ble_send_metadata();
            ble_send_csi_data();

#ifdef LV_VERSION_MAJOR
            lv_label_set_text_fmt(status_lbl, "Connected — ch %d rssi %d dBm",
                                  s_last_channel, s_last_rssi);
#endif
        }
#ifdef LV_VERSION_MAJOR
        lv_task_handler();
#endif
        vTaskDelay(pdMS_TO_TICKS(100)); /* ~10 Hz BLE notify rate */
    }
}

/* -------------------------------------------------------------------------
 * Public entry point
 * ------------------------------------------------------------------------- */
void ble_csi_app_start(void)
{
    const buddhas_config_t *cfg = settings_get();
    if (!cfg->ble_enabled) {
        ESP_LOGI(TAG, "BLE disabled in settings — skipping BLE CSI app");
        return;
    }

    /* Release classic BT memory — we only use BLE */
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    /* Register callbacks */
    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_event_handler));
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(GATTS_APP_ID));

    /* Set MTU to maximum for BLE 4.2+ (247 bytes data) */
    ESP_ERROR_CHECK(esp_ble_gatt_set_local_mtu(247));

    /* Start BLE CSI notification task */
    xTaskCreate(ble_csi_task, "ble_csi", 8192, NULL, 4, NULL);
}
