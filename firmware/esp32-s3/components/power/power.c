/**
 * power.c — AXP2101 power management IC driver (I²C)
 *
 * I²C address: 0x34
 *
 * Key registers used:
 *   0x00 - STATUS1      (charge/discharge flags)
 *   0x04 - BATFET       (battery FET control)
 *   0x26 - ADC_EN       (enable ADC channels)
 *   0x34 - VBAT_H/L     (battery voltage ADC)
 *   0x38 - IBUS_H/L     (USB input current ADC)
 *   0x3C - IBAT_H/L     (battery current ADC)
 *   0xB8 - COULOMB_CTL  (coulomb counter)
 *   0x64–0x6E           (DC-DC / LDO enable + voltage)
 */

#include "power.h"
#include <string.h>
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AXP2101_ADDR    0x34
#define I2C_PORT        I2C_NUM_0  /* Shared with IMU */

/* Register map (subset) */
#define REG_STATUS1     0x00
#define REG_ADC_EN      0x30
#define REG_VBAT_H      0x34
#define REG_VBAT_L      0x35
#define REG_IBAT_H      0x3C
#define REG_IBAT_L      0x3D
#define REG_SOC         0xA4
#define REG_DCDC1_CTL   0x80  /* DCDC1 on/off */
#define REG_LDO1_CTL    0x90  /* LDO1 on/off  */

static const char *TAG = "POWER";

/* ------------------------------------------------------------------ */

static esp_err_t axp_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_write_to_device(I2C_PORT, AXP2101_ADDR, buf, 2,
                                      pdMS_TO_TICKS(10));
}

static uint8_t axp_read(uint8_t reg)
{
    uint8_t val = 0;
    i2c_master_write_read_device(I2C_PORT, AXP2101_ADDR,
                                  &reg, 1, &val, 1,
                                  pdMS_TO_TICKS(10));
    return val;
}

/* ------------------------------------------------------------------ */

void power_init(void)
{
    /* Enable battery voltage + current ADC channels */
    axp_write(REG_ADC_EN, 0xFF);

    /* Verify comms */
    uint8_t status = axp_read(REG_STATUS1);
    ESP_LOGI(TAG, "AXP2101 STATUS1 = 0x%02X (%s)",
             status, (status & 0x80) ? "charging" : "discharging");

    ESP_LOGI(TAG, "AXP2101 power manager initialised");
}

void power_get_status(power_status_t *out)
{
    if (!out) return;

    /* Battery voltage: 14-bit ADC, LSB = 1.1 mV */
    uint16_t vbat_raw = ((uint16_t)(axp_read(REG_VBAT_H) & 0x3F) << 8)
                       | axp_read(REG_VBAT_L);
    out->voltage_v = vbat_raw * 0.0011f;

    /* Battery current: 13-bit signed ADC, LSB = 1 mA */
    int16_t ibat_raw = (int16_t)(((uint16_t)(axp_read(REG_IBAT_H) & 0x1F) << 8)
                                 | axp_read(REG_IBAT_L));
    if (ibat_raw & 0x1000) ibat_raw |= 0xE000;  /* sign-extend 13-bit */
    out->current_ma = (float)ibat_raw;

    /* State of charge */
    out->soc_pct  = (int)axp_read(REG_SOC);
    out->charging = (axp_read(REG_STATUS1) & 0x80) != 0;
}

void power_set_rail(const char *rail, bool enabled)
{
    uint8_t reg = 0;
    if      (strcmp(rail, "dcdc1") == 0) reg = REG_DCDC1_CTL;
    else if (strcmp(rail, "ldo1")  == 0) reg = REG_LDO1_CTL;
    else {
        ESP_LOGW(TAG, "Unknown rail: %s", rail);
        return;
    }
    uint8_t val = axp_read(reg);
    if (enabled) val |= 0x01; else val &= ~0x01;
    axp_write(reg, val);
    ESP_LOGI(TAG, "Rail '%s' %s", rail, enabled ? "ON" : "OFF");
}

void power_set_mode(const char *mode)
{
    if (strcmp(mode, "sleep") == 0) {
        /* Disable display + audio rails to save power */
        power_set_rail("ldo1", false);
        ESP_LOGI(TAG, "Power mode: sleep");
    } else if (strcmp(mode, "monitor") == 0) {
        power_set_rail("ldo1", true);
        power_set_rail("dcdc1", true);
        ESP_LOGI(TAG, "Power mode: monitor");
    } else {
        /* active: all rails on */
        power_set_rail("ldo1", true);
        power_set_rail("dcdc1", true);
        ESP_LOGI(TAG, "Power mode: active");
    }
}
