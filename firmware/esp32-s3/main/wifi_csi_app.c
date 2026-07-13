/**
 * wifi_csi_app.c — Wi-Fi CSI capture, LVGL magnitude/phase heatmap, UDP + SD logging
 *
 * Features:
 *  - Captures all 52 CSI subcarriers via esp_wifi_set_csi_rx_cb()
 *  - Displays a live magnitude heatmap on the 2.06" AMOLED (LVGL)
 *  - Streams JSON-encoded CSI to up to 4 UDP endpoints (Jetson, laptop,
 *    Android, other ESP32 nodes) on port 5500
 *  - Logs CSI to SD card in JSON-Lines format with timestamps
 *  - Exposes start/stop controls and live statistics (RSSI, channel, rate)
 *
 * Data format (UDP + SD card):
 *   {"node_id":"watch_01","timestamp":<us>,"channel":<n>,"rssi":<dBm>,
 *    "rate":<Mbps>,"phases":[...],"magnitudes":[...]}
 */

#include "wifi_csi_app.h"
#include "settings_app.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

/* LVGL — conditionally compiled */
#if defined(LV_VERSION_MAJOR)
#  include "lvgl.h"
#else
typedef void lv_obj_t;
typedef void lv_chart_series_t;
#  define lv_chart_create(p)          NULL
#  define lv_chart_set_type(c,t)      (void)(c)
#  define lv_chart_set_point_count(c,n) (void)(c)
#  define lv_chart_add_series(c,col,ax) NULL
#  define lv_chart_set_next_value(c,s,v) (void)(c)
#  define lv_chart_refresh(c)         (void)(c)
#  define lv_label_create(p)          NULL
#  define lv_label_set_text_fmt(o,...) (void)(o)
#  define lv_obj_align(o,a,x,y)       (void)(o)
#  define lv_obj_set_size(o,w,h)      (void)(o)
#  define lv_btn_create(p)            NULL
#  define lv_obj_add_event_cb(o,c,e,d) (void)(o)
#  define lv_event_get_target(e)      NULL
#  define lv_scr_act()                NULL
#  define LV_CHART_TYPE_LINE          0
#  define LV_CHART_AXIS_PRIMARY_Y     0
#  define LV_COLOR_MAKE(r,g,b)        0
#  define LV_ALIGN_TOP_LEFT           0
#  define LV_ALIGN_BOTTOM_LEFT        1
#  define LV_EVENT_CLICKED            0
#  define lv_task_handler()           do {} while(0)
#endif

#define TAG             "WIFI_CSI_APP"
#define MAX_SUBCARRIERS 52
#define PKT_BUF_SIZE    2048
#define MAX_ENDPOINTS   4    /* simultaneous UDP streaming targets */
#define SD_LOG_PATH     "/sdcard/csi_log.jsonl"

/* UDP streaming endpoints (populated from settings + discovery) */
typedef struct {
    struct sockaddr_in addr;
    bool               active;
} udp_endpoint_t;

static udp_endpoint_t  s_endpoints[MAX_ENDPOINTS];
static int             s_udp_sock  = -1;
static bool            s_capturing = false;

/* Latest CSI snapshot (written in CSI ISR context, read in UI task) */
static volatile float  s_mag[MAX_SUBCARRIERS];
static volatile float  s_phase[MAX_SUBCARRIERS];
static volatile int    s_last_rssi    = 0;
static volatile int    s_last_channel = 0;
static volatile uint32_t s_last_rate  = 0;
static volatile int    s_pkt_count    = 0;

/* SD card file handle (NULL if SD not mounted) */
static FILE *s_sd_file = NULL;

/* -------------------------------------------------------------------------
 * UDP helpers
 * ------------------------------------------------------------------------- */
static void udp_init(void)
{
    if (s_udp_sock >= 0) return;

    s_udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_udp_sock < 0) {
        ESP_LOGE(TAG, "UDP socket creation failed");
        return;
    }

    /* Endpoint 0: Jetson (from settings) */
    const buddhas_config_t *cfg = settings_get();
    s_endpoints[0].addr.sin_family = AF_INET;
    s_endpoints[0].addr.sin_port   = htons(cfg->udp_port);
    inet_aton(cfg->jetson_ip, &s_endpoints[0].addr.sin_addr);
    s_endpoints[0].active = cfg->wifi_streaming;

    /* Endpoints 1–3 can be populated dynamically via add_udp_target() */
    ESP_LOGI(TAG, "UDP socket ready, primary target: %s:%d",
             cfg->jetson_ip, cfg->udp_port);
}

/**
 * wifi_csi_app_add_target — Register an additional UDP streaming target.
 * @param ip_str  Dotted-decimal IP string
 * @param port    UDP port number
 */
void wifi_csi_app_add_target(const char *ip_str, uint16_t port)
{
    for (int i = 1; i < MAX_ENDPOINTS; i++) {
        if (!s_endpoints[i].active) {
            s_endpoints[i].addr.sin_family = AF_INET;
            s_endpoints[i].addr.sin_port   = htons(port);
            inet_aton(ip_str, &s_endpoints[i].addr.sin_addr);
            s_endpoints[i].active = true;
            ESP_LOGI(TAG, "UDP target added: %s:%d", ip_str, port);
            return;
        }
    }
    ESP_LOGW(TAG, "No free endpoint slots");
}

static void udp_send(const char *buf, size_t len)
{
    if (s_udp_sock < 0) return;
    for (int i = 0; i < MAX_ENDPOINTS; i++) {
        if (s_endpoints[i].active) {
            sendto(s_udp_sock, buf, len, 0,
                   (struct sockaddr *)&s_endpoints[i].addr,
                   sizeof(s_endpoints[i].addr));
        }
    }
}

/* -------------------------------------------------------------------------
 * SD card logging
 * ------------------------------------------------------------------------- */
static void sd_open(void)
{
    const buddhas_config_t *cfg = settings_get();
    if (!cfg->logging_to_sd) return;

    s_sd_file = fopen(SD_LOG_PATH, "a");
    if (!s_sd_file) {
        ESP_LOGW(TAG, "SD card not available — logging to UART only");
    } else {
        ESP_LOGI(TAG, "SD log opened: %s", SD_LOG_PATH);
    }
}

static void sd_write(const char *line)
{
    if (!s_sd_file) return;
    fputs(line, s_sd_file);
    fputc('\n', s_sd_file);
    fflush(s_sd_file);
}

/* -------------------------------------------------------------------------
 * CSI callback (called in Wi-Fi task context)
 * ------------------------------------------------------------------------- */
static void csi_cb(void *ctx, wifi_csi_info_t *data)
{
    if (!data || !data->buf || !s_capturing) return;

    int n = data->len < MAX_SUBCARRIERS ? data->len : MAX_SUBCARRIERS;

    /* Update snapshot arrays */
    for (int i = 0; i < n; i++) {
        float re = (float)data->buf[i].real;
        float im = (float)data->buf[i].imag;
        s_mag[i]   = sqrtf(re * re + im * im);
        s_phase[i] = atan2f(im, re);
    }
    s_last_rssi    = data->rx_ctrl.rssi;
    s_last_channel = (int)data->rx_ctrl.primary_channel;
    s_last_rate    = data->rx_ctrl.rate;
    s_pkt_count++;

    /* Build JSON packet */
    char pkt[PKT_BUF_SIZE];
    const buddhas_config_t *cfg = settings_get();
    int off = snprintf(pkt, sizeof(pkt),
        "{\"node_id\":\"%s\",\"timestamp\":%llu,\"channel\":%d,"
        "\"rssi\":%d,\"rate\":%lu,\"phases\":[",
        cfg->node_id,
        (unsigned long long)esp_timer_get_time(),
        s_last_channel,
        s_last_rssi,
        (unsigned long)s_last_rate);

    for (int i = 0; i < n && off < (int)sizeof(pkt) - 100; i++) {
        off += snprintf(pkt + off, sizeof(pkt) - off,
            "%s%.4f", i > 0 ? "," : "", (float)s_phase[i]);
    }
    off += snprintf(pkt + off, sizeof(pkt) - off, "],\"magnitudes\":[");
    for (int i = 0; i < n && off < (int)sizeof(pkt) - 10; i++) {
        off += snprintf(pkt + off, sizeof(pkt) - off,
            "%s%.4f", i > 0 ? "," : "", (float)s_mag[i]);
    }
    snprintf(pkt + off, sizeof(pkt) - off, "]}");

    /* Stream over UDP */
    udp_send(pkt, strlen(pkt));

    /* Log to SD card */
    sd_write(pkt);
}

/* -------------------------------------------------------------------------
 * LVGL UI
 * ------------------------------------------------------------------------- */
#ifdef LV_VERSION_MAJOR

static lv_obj_t         *s_chart      = NULL;
static lv_chart_series_t *s_mag_ser   = NULL;
static lv_chart_series_t *s_phase_ser = NULL;
static lv_obj_t          *s_stats_lbl = NULL;
static lv_obj_t          *s_ctrl_btn  = NULL;

static void ctrl_btn_cb(lv_event_t *e)
{
    s_capturing = !s_capturing;
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    lv_label_set_text(lbl, s_capturing ? "Stop" : "Start");
    ESP_LOGI(TAG, "CSI capture %s", s_capturing ? "started" : "stopped");
}

static void build_csi_ui(void)
{
    lv_obj_t *scr = lv_scr_act();

    /* Title */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "WiFi CSI Monitor");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    /* Line chart for magnitude */
    s_chart = lv_chart_create(scr);
    lv_obj_set_size(s_chart, 230, 140);
    lv_obj_align(s_chart, LV_ALIGN_TOP_LEFT, 5, 28);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, MAX_SUBCARRIERS);

    s_mag_ser   = lv_chart_add_series(s_chart,
                      lv_palette_main(LV_PALETTE_BLUE), LV_CHART_AXIS_PRIMARY_Y);
    s_phase_ser = lv_chart_add_series(s_chart,
                      lv_palette_main(LV_PALETTE_RED),  LV_CHART_AXIS_PRIMARY_Y);

    /* Stats label */
    s_stats_lbl = lv_label_create(scr);
    lv_label_set_text(s_stats_lbl, "Ch: --  RSSI: -- dBm\nRate: -- Mbps  Pkts: 0");
    lv_obj_align(s_stats_lbl, LV_ALIGN_TOP_LEFT, 5, 175);

    /* Start/Stop button */
    s_ctrl_btn = lv_btn_create(scr);
    lv_obj_set_size(s_ctrl_btn, 80, 36);
    lv_obj_align(s_ctrl_btn, LV_ALIGN_BOTTOM_RIGHT, -5, -5);
    lv_obj_add_event_cb(s_ctrl_btn, ctrl_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn_lbl = lv_label_create(s_ctrl_btn);
    lv_label_set_text(btn_lbl, "Start");
    lv_obj_align(btn_lbl, LV_ALIGN_CENTER, 0, 0);
}

static void update_chart(void)
{
    if (!s_chart || !s_capturing) return;

    for (int i = 0; i < MAX_SUBCARRIERS; i++) {
        lv_chart_set_next_value(s_chart, s_mag_ser,   (lv_coord_t)(s_mag[i]));
        lv_chart_set_next_value(s_chart, s_phase_ser,
            (lv_coord_t)((s_phase[i] + 3.14159f) * 20.0f));
    }
    lv_chart_refresh(s_chart);

    lv_label_set_text_fmt(s_stats_lbl,
        "Ch: %d  RSSI: %d dBm\nRate: %lu Mbps  Pkts: %d",
        s_last_channel, s_last_rssi,
        (unsigned long)s_last_rate, s_pkt_count);
}

#endif /* LV_VERSION_MAJOR */

/* -------------------------------------------------------------------------
 * CSI app task
 * ------------------------------------------------------------------------- */
static void wifi_csi_task(void *pvParameters)
{
    ESP_LOGI(TAG, "WiFi CSI app task started");

    udp_init();
    sd_open();

    /* Register CSI callback */
    wifi_csi_config_t csi_cfg = {
        .lltf_en           = true,
        .htltf_en          = true,
        .stbc_htltf2_en    = true,
        .ltf_merge_en      = true,
        .channel_filter_en = true,
        .manu_scale        = false,
        .shift             = false,
    };
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(csi_cb, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));

#ifdef LV_VERSION_MAJOR
    build_csi_ui();
#endif

    s_capturing = true;

    while (1) {
#ifdef LV_VERSION_MAJOR
        update_chart();
        lv_task_handler();
#endif
        vTaskDelay(pdMS_TO_TICKS(50)); /* ~20 UI fps */
    }
}

void wifi_csi_app_start(void)
{
    xTaskCreate(wifi_csi_task, "wifi_csi", 8192, NULL, 4, NULL);
}
