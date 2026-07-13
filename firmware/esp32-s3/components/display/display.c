/**
 * display.c — 410×502 AMOLED display + LED + vibrator motor
 *
 * SPI-driven AMOLED (RM67162 or compatible), plus:
 *   GPIO_LED  — status LED
 *   GPIO_VIB  — vibrator motor (active-high via MOSFET)
 *
 * In a production build, swap the stub draw calls with your
 * LVGL/direct-register rendering code.
 */

#include "display.h"
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ---- pin definitions ---- */
#define GPIO_LED        4
#define GPIO_VIB        5
#define GPIO_DISP_DC    6
#define GPIO_DISP_RST   7
#define GPIO_DISP_CS    10
#define GPIO_DISP_MOSI  11
#define GPIO_DISP_CLK   12
#define SPI_HOST        SPI2_HOST
#define SPI_FREQ_HZ     (40 * 1000 * 1000)

static const char *TAG = "DISPLAY";
static spi_device_handle_t spi_dev = NULL;

/* ------------------------------------------------------------------ */
/*  SPI helpers                                                         */
/* ------------------------------------------------------------------ */
static void disp_spi_send(const uint8_t *data, size_t len, bool is_cmd)
{
    if (!spi_dev || len == 0) return;
    gpio_set_level(GPIO_DISP_DC, is_cmd ? 0 : 1);
    spi_transaction_t t = {
        .length    = len * 8,
        .tx_buffer = data,
    };
    spi_device_transmit(spi_dev, &t);
}

static inline void disp_cmd(uint8_t cmd)      { disp_spi_send(&cmd, 1, true);  }
static inline void disp_data(uint8_t d)       { disp_spi_send(&d,   1, false); }

/* ------------------------------------------------------------------ */
/*  Initialisation                                                      */
/* ------------------------------------------------------------------ */
void display_init(void)
{
    /* GPIO: LED + vibrator */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << GPIO_LED) | (1ULL << GPIO_VIB),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(GPIO_LED, 0);
    gpio_set_level(GPIO_VIB, 0);

    /* SPI bus + device */
    spi_bus_config_t bus = {
        .mosi_io_num   = GPIO_DISP_MOSI,
        .miso_io_num   = -1,
        .sclk_io_num   = GPIO_DISP_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = SPI_FREQ_HZ,
        .mode           = 0,
        .spics_io_num   = GPIO_DISP_CS,
        .queue_size     = 8,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI_HOST, &dev, &spi_dev));

    /* Hardware reset */
    gpio_set_direction(GPIO_DISP_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_DISP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(GPIO_DISP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* Minimal init sequence (RM67162): sleep out + display on */
    disp_cmd(0x11);  /* Sleep out */
    vTaskDelay(pdMS_TO_TICKS(120));
    disp_cmd(0x29);  /* Display on */

    ESP_LOGI(TAG, "AMOLED display initialised (410x502)");
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */
void display_show_alert(const char *severity, float freq_hz)
{
    if (strcmp(severity, "none") == 0) {
        gpio_set_level(GPIO_LED, 0);
        ESP_LOGI(TAG, "Alert cleared");
        return;
    }
    /* In a full LVGL build: lv_label_set_text(), lv_obj_set_style_bg_color(), etc.
     * Here we log + flash the LED as a minimal functional implementation. */
    ESP_LOGW(TAG, "ALERT [%s] @ %.0f Hz", severity, freq_hz);
    int flashes = strcmp(severity, "high") == 0 ? 5 : 2;
    for (int i = 0; i < flashes; i++) {
        gpio_set_level(GPIO_LED, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(GPIO_LED, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void display_show_status(const char *text)
{
    /* In a full LVGL build: lv_label_set_text(status_label, text). */
    ESP_LOGI(TAG, "STATUS: %s", text);
}

void display_show_coherence(float coherence, float baseline)
{
    ESP_LOGI(TAG, "COHERENCE: %.3f (baseline %.3f)", coherence, baseline);
}

void display_led_flash(const char *pattern)
{
    if (strcmp(pattern, "none") == 0) {
        gpio_set_level(GPIO_LED, 0);
        return;
    }
    if (strcmp(pattern, "solid") == 0) {
        gpio_set_level(GPIO_LED, 1);
        return;
    }
    /* pulse: 3 quick flashes */
    for (int i = 0; i < 3; i++) {
        gpio_set_level(GPIO_LED, 1);
        vTaskDelay(pdMS_TO_TICKS(80));
        gpio_set_level(GPIO_LED, 0);
        vTaskDelay(pdMS_TO_TICKS(80));
    }
}

void wifi_ctrl_vibrate(int duration_ms)
{
    gpio_set_level(GPIO_VIB, 1);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    gpio_set_level(GPIO_VIB, 0);
    ESP_LOGD(TAG, "Vibrated %d ms", duration_ms);
}
