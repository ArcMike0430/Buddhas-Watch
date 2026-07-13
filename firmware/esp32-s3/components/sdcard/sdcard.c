/**
 * sdcard.c — SD card logging via SPI + FatFS
 */

#include "sdcard.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "esp_timer.h"

/* ---- SPI pins — adapt to your PCB ---- */
#define SD_MOSI_PIN     35
#define SD_MISO_PIN     36
#define SD_CLK_PIN      37
#define SD_CS_PIN       38
#define SD_SPI_HOST     SPI3_HOST
#define MOUNT_POINT     "/sdcard"
#define LOG_FILE        MOUNT_POINT "/log.jsonl"

static const char *TAG = "SDCARD";
static sdmmc_card_t *card = NULL;
static bool mounted = false;

bool sdcard_init(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = true,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;

    spi_bus_config_t bus = {
        .mosi_io_num   = SD_MOSI_PIN,
        .miso_io_num   = SD_MISO_PIN,
        .sclk_io_num   = SD_CLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    esp_err_t ret = spi_bus_initialize(SD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SPI bus init error: %s (may already be initialised)", esp_err_to_name(ret));
    }

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs   = SD_CS_PIN;
    slot.host_id   = SD_SPI_HOST;

    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot, &mount_cfg, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        return false;
    }

    mounted = true;
    sdmmc_card_print_info(stdout, card);
    ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_POINT);
    return true;
}

void sdcard_write_log_marker(const char *event, const char *json_detail)
{
    if (!mounted) {
        ESP_LOGW(TAG, "SD not mounted — skipping marker: %s", event);
        return;
    }

    FILE *f = fopen(LOG_FILE, "a");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for append", LOG_FILE);
        return;
    }

    int64_t ts_s = esp_timer_get_time() / 1000000LL;
    fprintf(f, "{\"ts\":%lld,\"event\":\"%s\",\"detail\":%s}\n",
            ts_s, event, json_detail ? json_detail : "{}");
    fclose(f);

    ESP_LOGD(TAG, "Log marker: %s", event);
}

void sdcard_deinit(void)
{
    if (mounted) {
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
        mounted = false;
        card    = NULL;
        ESP_LOGI(TAG, "SD card unmounted");
    }
}
