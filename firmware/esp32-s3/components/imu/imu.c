/**
 * imu.c — QMI8658 6-axis IMU driver (I²C)
 *
 * Register map excerpt used here:
 *   0x00 - WHO_AM_I   (should read 0x05)
 *   0x02 - CTRL1      (accelerometer ODR + range)
 *   0x03 - CTRL2      (gyroscope ODR + range)
 *   0x08 - CTRL7      (enable accel + gyro)
 *   0x35 - AX_L … 0x40 - GZ_H  (6-axis output registers)
 */

#include "imu.h"
#include <string.h>
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define QMI8658_ADDR        0x6B
#define I2C_PORT            I2C_NUM_0
#define I2C_SDA_PIN         21
#define I2C_SCL_PIN         22
#define I2C_FREQ_HZ         400000

/* Register addresses */
#define REG_WHO_AM_I        0x00
#define REG_CTRL1           0x02
#define REG_CTRL2           0x03
#define REG_CTRL7           0x08
#define REG_DATA_BASE       0x35  /* AX_L, AX_H, AY_L, AY_H, AZ_L, AZ_H,
                                     GX_L, GX_H, GY_L, GY_H, GZ_L, GZ_H */

/* Sensitivity — ±8 g / 4096 LSB/g, ±512 dps / 64 LSB/dps */
#define ACC_SENSITIVITY     4096.0f
#define GYR_SENSITIVITY     64.0f

#define MOTION_BUF_LEN      64

static const char *TAG = "IMU";

static float motion_mag_buf[MOTION_BUF_LEN];
static int   motion_buf_idx = 0;

/* ------------------------------------------------------------------ */

static esp_err_t i2c_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_write_to_device(I2C_PORT, QMI8658_ADDR, buf, 2,
                                      pdMS_TO_TICKS(10));
}

static esp_err_t i2c_read_regs(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_write_read_device(I2C_PORT, QMI8658_ADDR,
                                        &reg, 1, out, len,
                                        pdMS_TO_TICKS(10));
}

/* ------------------------------------------------------------------ */

void imu_init(void)
{
    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = I2C_SDA_PIN,
        .scl_io_num       = I2C_SCL_PIN,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0));

    uint8_t who_am_i = 0;
    i2c_read_regs(REG_WHO_AM_I, &who_am_i, 1);
    if (who_am_i != 0x05) {
        ESP_LOGW(TAG, "WHO_AM_I = 0x%02X (expected 0x05) — check wiring", who_am_i);
    }

    /* CTRL1: accel ODR=256 Hz, ±8 g */
    i2c_write_reg(REG_CTRL1, 0x23);
    /* CTRL2: gyro ODR=256 Hz, ±512 dps */
    i2c_write_reg(REG_CTRL2, 0x53);
    /* CTRL7: enable accel + gyro */
    i2c_write_reg(REG_CTRL7, 0x03);

    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI(TAG, "QMI8658 initialised (WHO_AM_I=0x%02X)", who_am_i);
}

bool imu_read(imu_sample_t *out)
{
    uint8_t raw[12];
    if (i2c_read_regs(REG_DATA_BASE, raw, 12) != ESP_OK) return false;

    int16_t ax = (int16_t)((raw[1] << 8) | raw[0]);
    int16_t ay = (int16_t)((raw[3] << 8) | raw[2]);
    int16_t az = (int16_t)((raw[5] << 8) | raw[4]);
    int16_t gx = (int16_t)((raw[7] << 8) | raw[6]);
    int16_t gy = (int16_t)((raw[9] << 8) | raw[8]);
    int16_t gz = (int16_t)((raw[11] << 8) | raw[10]);

    out->acc_x_g   = ax / ACC_SENSITIVITY;
    out->acc_y_g   = ay / ACC_SENSITIVITY;
    out->acc_z_g   = az / ACC_SENSITIVITY;
    out->gyr_x_dps = gx / GYR_SENSITIVITY;
    out->gyr_y_dps = gy / GYR_SENSITIVITY;
    out->gyr_z_dps = gz / GYR_SENSITIVITY;
    out->timestamp_us = (int32_t)esp_timer_get_time();

    /* Update motion magnitude ring buffer */
    float mag = sqrtf(out->acc_x_g * out->acc_x_g +
                      out->acc_y_g * out->acc_y_g +
                      out->acc_z_g * out->acc_z_g);
    motion_mag_buf[motion_buf_idx % MOTION_BUF_LEN] = mag;
    motion_buf_idx++;

    return true;
}

bool imu_motion_detected(float threshold_g, int motion_window_ms)
{
    /* Number of samples covering motion_window_ms at ~256 Hz */
    int samples = (motion_window_ms * 256) / 1000;
    if (samples > MOTION_BUF_LEN) samples = MOTION_BUF_LEN;

    int start = motion_buf_idx - samples;
    if (start < 0) start = 0;

    for (int i = start; i < motion_buf_idx && i < start + samples; i++) {
        float mag = motion_mag_buf[i % MOTION_BUF_LEN];
        /* Subtract 1g gravity component (static baseline) */
        if (fabsf(mag - 1.0f) > threshold_g) return true;
    }
    return false;
}
