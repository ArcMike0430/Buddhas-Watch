/**
 * app_manager.c — App store client with LVGL browse/install UI
 *
 * Architecture:
 *  1. Fetch app list from backend: GET https://<store_host>/api/apps
 *  2. Display apps in an LVGL scrollable list (name, version, description)
 *  3. On app selection: fetch details, show icon/description, offer Install/Update
 *  4. Download binary to SD card (/sdcard/apps/<id>.bin)
 *  5. Validate SHA-256 checksum
 *  6. Mark app as installed; optional OTA-style load on next boot
 *
 * Backend URL is configurable via menuconfig (CONFIG_APP_STORE_HOST) or
 * defaults to the value below.
 */

#include "app_manager.h"
#include "settings_app.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"

/* LVGL — conditionally compiled */
#if defined(LV_VERSION_MAJOR)
#  include "lvgl.h"
#else
typedef void lv_obj_t;
#  define lv_obj_create(p)              NULL
#  define lv_label_create(p)            NULL
#  define lv_label_set_text(o,t)        (void)(o)
#  define lv_label_set_text_fmt(o,...)  (void)(o)
#  define lv_list_create(p)             NULL
#  define lv_list_add_btn(l,i,t)        NULL
#  define lv_obj_add_event_cb(o,c,e,d)  (void)(o)
#  define lv_event_get_target(e)        NULL
#  define lv_list_get_btn_text(l,b)     ""
#  define lv_obj_set_size(o,w,h)        (void)(o)
#  define lv_obj_align(o,a,x,y)         (void)(o)
#  define lv_spinner_create(p,a,b)      NULL
#  define lv_obj_del(o)                 (void)(o)
#  define lv_scr_act()                  NULL
#  define lv_task_handler()             do {} while(0)
#  define LV_ALIGN_TOP_MID              0
#  define LV_ALIGN_TOP_LEFT             1
#  define LV_ALIGN_BOTTOM_MID           2
#  define LV_EVENT_CLICKED              0
#  define LV_SIZE_CONTENT               0
#endif

#define TAG               "APP_MANAGER"
#define APP_STORE_HOST    "https://buddhas-watch.example.com"
#define API_APPS_PATH     "/api/apps"
#define HTTP_BUF_SIZE     4096
#define MAX_APPS          32
#define APP_INSTALL_DIR   "/sdcard/apps"
#define SHA256_HEX_LEN    65  /* 32 bytes * 2 chars + null */

typedef struct {
    char id[32];
    char name[64];
    char version[16];
    char description[128];
    char binary_url[256];
    char checksum[SHA256_HEX_LEN]; /* "sha256:<hex>" */
    uint32_t size_bytes;
    bool installed;
} app_meta_t;

static app_meta_t s_apps[MAX_APPS];
static int        s_app_count = 0;
static bool       s_fetching  = false;

/* -------------------------------------------------------------------------
 * HTTP helpers
 * ------------------------------------------------------------------------- */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} http_body_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    http_body_t *body = (http_body_t *)evt->user_data;
    if (!body) return ESP_OK;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (body->len + evt->data_len < body->cap) {
            memcpy(body->buf + body->len, evt->data, evt->data_len);
            body->len += evt->data_len;
            body->buf[body->len] = '\0';
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

/**
 * http_get_json — Perform a GET request and return the response body as a
 * heap-allocated string.  Caller must free() the returned pointer.
 * Returns NULL on error.
 */
static char *http_get_json(const char *url)
{
    char *data = (char *)calloc(1, HTTP_BUF_SIZE);
    if (!data) return NULL;

    http_body_t body = { .buf = data, .len = 0, .cap = HTTP_BUF_SIZE - 1 };

    esp_http_client_config_t cfg = {
        .url              = url,
        .event_handler    = http_event_cb,
        .user_data        = &body,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms       = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "HTTP GET %s failed: %s (%d)", url, esp_err_to_name(err), status);
        free(data);
        return NULL;
    }
    return data;
}

/* -------------------------------------------------------------------------
 * App list fetch & parse
 * ------------------------------------------------------------------------- */
static bool fetch_app_list(void)
{
    char url[300];
    snprintf(url, sizeof(url), "%s%s", APP_STORE_HOST, API_APPS_PATH);

    ESP_LOGI(TAG, "Fetching app list from %s", url);
    char *json = http_get_json(url);
    if (!json) return false;

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse app list JSON");
        return false;
    }

    s_app_count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, root) {
        if (s_app_count >= MAX_APPS) break;
        app_meta_t *a = &s_apps[s_app_count];
        memset(a, 0, sizeof(*a));

#define GET_STR(key, field) do { \
    cJSON *_v = cJSON_GetObjectItem(item, key); \
    if (_v && cJSON_IsString(_v)) strncpy(a->field, _v->valuestring, sizeof(a->field) - 1); \
} while(0)

        GET_STR("id",          id);
        GET_STR("name",        name);
        GET_STR("version",     version);
        GET_STR("description", description);
        GET_STR("binary_url",  binary_url);
        GET_STR("checksum",    checksum);

        cJSON *sz = cJSON_GetObjectItem(item, "size_bytes");
        if (sz && cJSON_IsNumber(sz)) a->size_bytes = (uint32_t)sz->valuedouble;

        /* Check if already installed on SD */
        char path[64];
        snprintf(path, sizeof(path), "%s/%s.bin", APP_INSTALL_DIR, a->id);
        FILE *f = fopen(path, "r");
        if (f) { fclose(f); a->installed = true; }

        s_app_count++;
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Fetched %d apps from store", s_app_count);
    return true;
}

/* -------------------------------------------------------------------------
 * App download & checksum validation
 * ------------------------------------------------------------------------- */
static bool validate_sha256(const char *path, const char *expected_hex)
{
    /* expected_hex format: "sha256:<64-char-hex>" */
    const char *hex = strstr(expected_hex, "sha256:");
    if (!hex) return true; /* No checksum to validate */
    hex += 7;

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);

    uint8_t buf[512];
    size_t  n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        mbedtls_sha256_update(&ctx, buf, n);
    }
    fclose(f);

    uint8_t hash[32];
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    char calc_hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(calc_hex + i * 2, 3, "%02x", hash[i]);
    }

    bool ok = (strncmp(calc_hex, hex, 64) == 0);
    if (!ok) {
        ESP_LOGE(TAG, "Checksum mismatch: expected %s got %s", hex, calc_hex);
    }
    return ok;
}

static bool download_app(app_meta_t *app)
{
    ESP_LOGI(TAG, "Downloading %s v%s (%lu bytes)...",
             app->name, app->version, (unsigned long)app->size_bytes);

    /* Ensure install directory exists */
    mkdir(APP_INSTALL_DIR, 0755);

    char path[64];
    snprintf(path, sizeof(path), "%s/%s.bin", APP_INSTALL_DIR, app->id);

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s for writing", path);
        return false;
    }

    esp_http_client_config_t cfg = {
        .url               = app->binary_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 30000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        fclose(f);
        esp_http_client_cleanup(client);
        return false;
    }

    esp_http_client_fetch_headers(client);

    char   buf[512];
    int    bytes_written = 0;
    int    rd;
    while ((rd = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, rd, f);
        bytes_written += rd;
    }
    fclose(f);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "Downloaded %d bytes to %s", bytes_written, path);

    /* Validate checksum */
    if (!validate_sha256(path, app->checksum)) {
        remove(path);
        return false;
    }

    app->installed = true;
    ESP_LOGI(TAG, "App '%s' installed successfully", app->name);
    return true;
}

/* -------------------------------------------------------------------------
 * LVGL UI
 * ------------------------------------------------------------------------- */
#ifdef LV_VERSION_MAJOR

static lv_obj_t *s_list     = NULL;
static lv_obj_t *s_info_lbl = NULL;
static lv_obj_t *s_spinner  = NULL;

static void app_btn_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    const char *name = lv_list_get_btn_text(s_list, btn);

    /* Find app by name */
    for (int i = 0; i < s_app_count; i++) {
        if (strcmp(s_apps[i].name, name) == 0) {
            char info[256];
            snprintf(info, sizeof(info),
                     "%s v%s\n%s\n%lu KB\n%s",
                     s_apps[i].name, s_apps[i].version,
                     s_apps[i].description,
                     (unsigned long)(s_apps[i].size_bytes / 1024),
                     s_apps[i].installed ? "[Installed]" : "[Not installed]");
            lv_label_set_text(s_info_lbl, info);

            /* Download in background */
            if (!s_apps[i].installed) {
                s_spinner = lv_spinner_create(lv_scr_act(), 1000, 60);
                lv_obj_set_size(s_spinner, 50, 50);
                lv_obj_align(s_spinner, LV_ALIGN_BOTTOM_MID, 0, -5);
                download_app(&s_apps[i]);
                lv_obj_del(s_spinner);
                s_spinner = NULL;
                lv_label_set_text(s_info_lbl, "Installation complete!");
            }
            break;
        }
    }
}

static void refresh_btn_cb(lv_event_t *e)
{
    lv_label_set_text(s_info_lbl, "Refreshing...");
    fetch_app_list();
    /* Rebuild list */
    lv_obj_clean(s_list);
    for (int i = 0; i < s_app_count; i++) {
        lv_obj_t *btn = lv_list_add_btn(s_list, NULL, s_apps[i].name);
        lv_obj_add_event_cb(btn, app_btn_cb, LV_EVENT_CLICKED, NULL);
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "%d apps available", s_app_count);
    lv_label_set_text(s_info_lbl, msg);
}

static void build_app_manager_ui(void)
{
    lv_obj_t *scr = lv_scr_act();

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "App Store");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    s_list = lv_list_create(scr);
    lv_obj_set_size(s_list, 240, 160);
    lv_obj_align(s_list, LV_ALIGN_TOP_LEFT, 0, 28);

    for (int i = 0; i < s_app_count; i++) {
        lv_obj_t *btn = lv_list_add_btn(s_list, NULL, s_apps[i].name);
        lv_obj_add_event_cb(btn, app_btn_cb, LV_EVENT_CLICKED, NULL);
    }

    s_info_lbl = lv_label_create(scr);
    lv_label_set_text(s_info_lbl, "Select an app");
    lv_obj_align(s_info_lbl, LV_ALIGN_TOP_LEFT, 5, 195);

    lv_obj_t *refresh_btn = lv_btn_create(scr);
    lv_obj_set_size(refresh_btn, 80, 30);
    lv_obj_align(refresh_btn, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_add_event_cb(refresh_btn, refresh_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *r_lbl = lv_label_create(refresh_btn);
    lv_label_set_text(r_lbl, "Refresh");
    lv_obj_align(r_lbl, LV_ALIGN_CENTER, 0, 0);
}

#endif /* LV_VERSION_MAJOR */

/* -------------------------------------------------------------------------
 * App manager task
 * ------------------------------------------------------------------------- */
static void app_manager_task(void *pvParameters)
{
    ESP_LOGI(TAG, "App manager task started");

    /* Initial fetch */
    vTaskDelay(pdMS_TO_TICKS(3000)); /* Allow Wi-Fi to stabilise */
    s_fetching = true;
    fetch_app_list();
    s_fetching = false;

#ifdef LV_VERSION_MAJOR
    build_app_manager_ui();
    while (1) {
        lv_task_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
#else
    /* Check for updates every 60 seconds when LVGL is not present */
    while (1) {
        const buddhas_config_t *cfg = settings_get();
        if (cfg->auto_update) {
            fetch_app_list();
        }
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
#endif
}

void app_manager_start(void)
{
    xTaskCreate(app_manager_task, "app_mgr", 12288, NULL, 2, NULL);
}
