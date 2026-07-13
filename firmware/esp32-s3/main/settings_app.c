/**
 * settings_app.c — Persistent device configuration + LVGL settings UI
 *
 * Provides:
 *  - NVS-backed storage for all device settings
 *  - LVGL tab-based settings screen:
 *      Tab 1: Network  (Wi-Fi SSID/password, Jetson IP, UDP port)
 *      Tab 2: Display  (brightness slider, timeout)
 *      Tab 3: BLE      (enable/disable, pairing)
 *      Tab 4: Logging  (SD card, Wi-Fi streaming)
 *      Tab 5: About    (firmware version, node ID, storage usage)
 *
 * LVGL must be initialised (display + touch driver) before this task runs.
 */

#include "settings_app.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"

/* LVGL — include when the LVGL component is added to the project */
#if defined(CONFIG_LV_USE_DEMO_WIDGETS) || defined(LV_LVGL_H_INCLUDE_SIMPLE)
#  include "lvgl.h"
#else
/* Minimal stub types so this file compiles even without LVGL in the build */
typedef void lv_obj_t;
typedef void lv_event_t;
#  define lv_obj_create(p)          NULL
#  define lv_tabview_create(p,d,s)  NULL
#  define lv_tabview_add_tab(t,n)   NULL
#  define lv_label_create(p)        NULL
#  define lv_label_set_text(o,t)    (void)(o)
#  define lv_btn_create(p)          NULL
#  define lv_obj_set_size(o,w,h)    (void)(o)
#  define lv_obj_align(o,a,x,y)     (void)(o)
#  define lv_obj_add_event_cb(o,cb,e,d) (void)(o)
#  define lv_slider_create(p)       NULL
#  define lv_slider_set_value(o,v,a) (void)(o)
#  define lv_switch_create(p)       NULL
#  define lv_obj_add_state(o,s)     (void)(o)
#  define LV_EVENT_CLICKED          0
#  define LV_EVENT_VALUE_CHANGED    1
#  define LV_ALIGN_CENTER           0
#  define LV_STATE_CHECKED          0x0001
#endif

#define NVS_NAMESPACE   "bwatch_cfg"
#define TAG             "SETTINGS"

#define FW_VERSION      "1.0.0"

/* In-memory config (single copy, guarded by FreeRTOS task context) */
static buddhas_config_t s_cfg;
static bool             s_loaded = false;

/* -------------------------------------------------------------------------
 * Default values
 * ------------------------------------------------------------------------- */
static void apply_defaults(buddhas_config_t *cfg)
{
    strncpy(cfg->wifi_ssid,       "Buddhas-Net",    sizeof(cfg->wifi_ssid)     - 1);
    strncpy(cfg->wifi_password,   "your_password",  sizeof(cfg->wifi_password) - 1);
    strncpy(cfg->node_id,         "watch_01",       sizeof(cfg->node_id)       - 1);
    strncpy(cfg->jetson_ip,       "192.168.1.100",  sizeof(cfg->jetson_ip)     - 1);
    cfg->udp_port           = 5500;
    cfg->display_brightness = 200;
    cfg->display_timeout_s  = 30;
    cfg->ble_enabled        = true;
    cfg->logging_to_sd      = true;
    cfg->wifi_streaming     = true;
    cfg->auto_update        = false;
}

/* -------------------------------------------------------------------------
 * NVS load / save
 * ------------------------------------------------------------------------- */
void settings_load(buddhas_config_t *cfg)
{
    apply_defaults(cfg);

    nvs_handle_t h;
    esp_err_t    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed (%s) — using defaults", esp_err_to_name(err));
        return;
    }

#define NVS_GET_STR(key, field) \
    do { \
        size_t len = sizeof(cfg->field); \
        nvs_get_str(h, key, cfg->field, &len); \
    } while (0)

#define NVS_GET_U8(key, field)  nvs_get_u8(h,  key, &cfg->field)
#define NVS_GET_U16(key, field) nvs_get_u16(h, key, &cfg->field)
#define NVS_GET_I8(key, field)  \
    do { \
        int8_t _v = (int8_t)cfg->field; \
        nvs_get_i8(h, key, &_v); \
        cfg->field = (bool)_v; \
    } while(0)

    NVS_GET_STR("wifi_ssid",   wifi_ssid);
    NVS_GET_STR("wifi_pass",   wifi_password);
    NVS_GET_STR("node_id",     node_id);
    NVS_GET_STR("jetson_ip",   jetson_ip);
    NVS_GET_U16("udp_port",    udp_port);
    NVS_GET_U8 ("disp_bright", display_brightness);
    NVS_GET_U16("disp_tmout",  display_timeout_s);
    NVS_GET_I8 ("ble_en",      ble_enabled);
    NVS_GET_I8 ("log_sd",      logging_to_sd);
    NVS_GET_I8 ("wifi_stream", wifi_streaming);
    NVS_GET_I8 ("auto_upd",    auto_update);

    nvs_close(h);
    ESP_LOGI(TAG, "Config loaded: ssid=%s node=%s", cfg->wifi_ssid, cfg->node_id);
}

void settings_save(const buddhas_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open for write failed: %s", esp_err_to_name(err));
        return;
    }

    nvs_set_str(h, "wifi_ssid",   cfg->wifi_ssid);
    nvs_set_str(h, "wifi_pass",   cfg->wifi_password);
    nvs_set_str(h, "node_id",     cfg->node_id);
    nvs_set_str(h, "jetson_ip",   cfg->jetson_ip);
    nvs_set_u16(h, "udp_port",    cfg->udp_port);
    nvs_set_u8 (h, "disp_bright", cfg->display_brightness);
    nvs_set_u16(h, "disp_tmout",  cfg->display_timeout_s);
    nvs_set_i8 (h, "ble_en",      (int8_t)cfg->ble_enabled);
    nvs_set_i8 (h, "log_sd",      (int8_t)cfg->logging_to_sd);
    nvs_set_i8 (h, "wifi_stream", (int8_t)cfg->wifi_streaming);
    nvs_set_i8 (h, "auto_upd",    (int8_t)cfg->auto_update);

    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Config saved");
}

const buddhas_config_t *settings_get(void)
{
    return &s_cfg;
}

/* -------------------------------------------------------------------------
 * LVGL UI callbacks
 * ------------------------------------------------------------------------- */
#ifdef LV_VERSION_MAJOR   /* Only compile when LVGL is present */

static void save_btn_cb(lv_event_t *e)
{
    settings_save(&s_cfg);
    ESP_LOGI(TAG, "Settings saved via UI");
}

static void brightness_cb(lv_event_t *e)
{
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    s_cfg.display_brightness = (uint8_t)lv_slider_get_value(slider);
    /* TODO: apply brightness to CO5300 display backlight via ledc_set_duty() */
}

static void ble_sw_cb(lv_event_t *e)
{
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    s_cfg.ble_enabled = (lv_obj_get_state(sw) & LV_STATE_CHECKED) != 0;
}

static void log_sd_sw_cb(lv_event_t *e)
{
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    s_cfg.logging_to_sd = (lv_obj_get_state(sw) & LV_STATE_CHECKED) != 0;
}

/* Build the settings UI on the LVGL display */
static void build_settings_ui(void)
{
    lv_obj_t *tabview = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 40);

    /* --- Tab 1: Network --- */
    lv_obj_t *tab_net = lv_tabview_add_tab(tabview, "Network");

    lv_obj_t *lbl_ssid = lv_label_create(tab_net);
    lv_label_set_text(lbl_ssid, "WiFi SSID:");
    lv_obj_align(lbl_ssid, LV_ALIGN_TOP_LEFT, 5, 10);

    /* Text area for SSID — requires lv_textarea component */
    lv_obj_t *ta_ssid = lv_textarea_create(tab_net);
    lv_textarea_set_text(ta_ssid, s_cfg.wifi_ssid);
    lv_textarea_set_one_line(ta_ssid, true);
    lv_obj_set_size(ta_ssid, 160, 36);
    lv_obj_align(ta_ssid, LV_ALIGN_TOP_LEFT, 5, 35);

    lv_obj_t *lbl_pass = lv_label_create(tab_net);
    lv_label_set_text(lbl_pass, "Password:");
    lv_obj_align(lbl_pass, LV_ALIGN_TOP_LEFT, 5, 80);

    lv_obj_t *ta_pass = lv_textarea_create(tab_net);
    lv_textarea_set_text(ta_pass, s_cfg.wifi_password);
    lv_textarea_set_one_line(ta_pass, true);
    lv_textarea_set_password_mode(ta_pass, true);
    lv_obj_set_size(ta_pass, 160, 36);
    lv_obj_align(ta_pass, LV_ALIGN_TOP_LEFT, 5, 105);

    lv_obj_t *lbl_jetson = lv_label_create(tab_net);
    lv_label_set_text(lbl_jetson, "Jetson IP:");
    lv_obj_align(lbl_jetson, LV_ALIGN_TOP_LEFT, 5, 150);

    lv_obj_t *ta_jetson = lv_textarea_create(tab_net);
    lv_textarea_set_text(ta_jetson, s_cfg.jetson_ip);
    lv_textarea_set_one_line(ta_jetson, true);
    lv_obj_set_size(ta_jetson, 160, 36);
    lv_obj_align(ta_jetson, LV_ALIGN_TOP_LEFT, 5, 175);

    /* --- Tab 2: Display --- */
    lv_obj_t *tab_disp = lv_tabview_add_tab(tabview, "Display");

    lv_obj_t *lbl_bright = lv_label_create(tab_disp);
    lv_label_set_text(lbl_bright, "Brightness:");
    lv_obj_align(lbl_bright, LV_ALIGN_TOP_LEFT, 5, 10);

    lv_obj_t *sl_bright = lv_slider_create(tab_disp);
    lv_slider_set_range(sl_bright, 20, 255);
    lv_slider_set_value(sl_bright, s_cfg.display_brightness, LV_ANIM_OFF);
    lv_obj_set_size(sl_bright, 180, 20);
    lv_obj_align(sl_bright, LV_ALIGN_TOP_LEFT, 5, 40);
    lv_obj_add_event_cb(sl_bright, brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* --- Tab 3: BLE --- */
    lv_obj_t *tab_ble = lv_tabview_add_tab(tabview, "BLE");

    lv_obj_t *lbl_ble = lv_label_create(tab_ble);
    lv_label_set_text(lbl_ble, "BLE Enabled:");
    lv_obj_align(lbl_ble, LV_ALIGN_TOP_LEFT, 5, 10);

    lv_obj_t *sw_ble = lv_switch_create(tab_ble);
    lv_obj_align(sw_ble, LV_ALIGN_TOP_LEFT, 5, 40);
    if (s_cfg.ble_enabled) lv_obj_add_state(sw_ble, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_ble, ble_sw_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* --- Tab 4: Logging --- */
    lv_obj_t *tab_log = lv_tabview_add_tab(tabview, "Logging");

    lv_obj_t *lbl_sd = lv_label_create(tab_log);
    lv_label_set_text(lbl_sd, "Log to SD:");
    lv_obj_align(lbl_sd, LV_ALIGN_TOP_LEFT, 5, 10);

    lv_obj_t *sw_sd = lv_switch_create(tab_log);
    lv_obj_align(sw_sd, LV_ALIGN_TOP_LEFT, 5, 40);
    if (s_cfg.logging_to_sd) lv_obj_add_state(sw_sd, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_sd, log_sd_sw_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *lbl_wifi_stream = lv_label_create(tab_log);
    lv_label_set_text(lbl_wifi_stream, "WiFi Stream:");
    lv_obj_align(lbl_wifi_stream, LV_ALIGN_TOP_LEFT, 5, 90);

    /* --- Tab 5: About --- */
    lv_obj_t *tab_about = lv_tabview_add_tab(tabview, "About");

    char about_text[128];
    snprintf(about_text, sizeof(about_text),
             "Buddhas-Watch\nFW: %s\nNode: %s\nESP32-S3R8\n8MB PSRAM\n32MB Flash",
             FW_VERSION, s_cfg.node_id);
    lv_obj_t *lbl_about = lv_label_create(tab_about);
    lv_label_set_text(lbl_about, about_text);
    lv_obj_align(lbl_about, LV_ALIGN_TOP_LEFT, 5, 10);

    /* Save button (shared across tabs — floating at bottom) */
    lv_obj_t *btn_save = lv_btn_create(lv_scr_act());
    lv_obj_set_size(btn_save, 100, 36);
    lv_obj_align(btn_save, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_add_event_cb(btn_save, save_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_save = lv_label_create(btn_save);
    lv_label_set_text(lbl_save, "Save");
    lv_obj_align(lbl_save, LV_ALIGN_CENTER, 0, 0);
}

#endif /* LV_VERSION_MAJOR */

/* -------------------------------------------------------------------------
 * Settings task
 * ------------------------------------------------------------------------- */
static void settings_task(void *pvParameters)
{
    settings_load(&s_cfg);
    s_loaded = true;
    ESP_LOGI(TAG, "Settings task started");

#ifdef LV_VERSION_MAJOR
    build_settings_ui();

    /* LVGL tick loop — the display driver must call lv_tick_inc() from a
     * 1 ms hardware timer.  This task drives lv_task_handler(). */
    while (1) {
        lv_task_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
#else
    /* Without LVGL, simply keep the task alive so settings remain loaded */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif
}

void settings_app_start(void)
{
    xTaskCreate(settings_task, "settings_app", 8192, NULL, 3, NULL);
}
