/**
 * audio.c — ES7210 dual-microphone ADC driver (I²S + I²C control)
 *
 * I²C address: 0x40 (ADDR pin low)
 * I²S: standard Philips mode, 48 kHz, 16-bit, stereo
 */

#include "audio.h"
#include <string.h>
#include <math.h>
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ES7210_ADDR     0x40
#define I2C_PORT        I2C_NUM_0

/* ES7210 register addresses */
#define ES7210_RESET    0x00
#define ES7210_CLK_DIV  0x06
#define ES7210_ADC_CTRL 0x43
#define ES7210_MIC_GAIN 0x48   /* channels 1-2 gain */

/* I²S pins — adapt to your PCB */
#define I2S_BCLK_PIN    14
#define I2S_WS_PIN      15
#define I2S_DIN_PIN     16
#define I2S_PORT_NUM    I2S_NUM_0

static const char *TAG = "AUDIO";
static i2s_chan_handle_t i2s_rx_handle = NULL;
static int16_t rms_buf[AUDIO_BUF_SAMPLES];

/* ------------------------------------------------------------------ */

static esp_err_t es7210_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_write_to_device(I2C_PORT, ES7210_ADDR, buf, 2,
                                      pdMS_TO_TICKS(10));
}

/* ------------------------------------------------------------------ */

void audio_init(void)
{
    /* Configure ES7210 via I²C (I²C already initialised by IMU component) */
    es7210_write(ES7210_RESET, 0xFF);   /* Software reset */
    vTaskDelay(pdMS_TO_TICKS(10));
    es7210_write(ES7210_RESET, 0x00);   /* Release reset */
    es7210_write(ES7210_CLK_DIV, 0x01); /* MCLK div = 1 */
    es7210_write(ES7210_ADC_CTRL, 0x0C); /* Enable CH1+CH2 */
    es7210_write(ES7210_MIC_GAIN, 0x10); /* +16 dB mic gain */

    /* Configure I²S peripheral */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT_NUM,
                                                            I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &i2s_rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                     I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .bclk = I2S_BCLK_PIN,
            .ws   = I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_DIN_PIN,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_rx_handle));

    ESP_LOGI(TAG, "ES7210 + I²S initialised (%d Hz, stereo, 16-bit)", AUDIO_SAMPLE_RATE);
}

size_t audio_read(int16_t *buf, size_t samples)
{
    if (!i2s_rx_handle) return 0;
    size_t bytes_read = 0;
    i2s_channel_read(i2s_rx_handle, buf, samples * sizeof(int16_t),
                     &bytes_read, pdMS_TO_TICKS(100));
    return bytes_read / sizeof(int16_t);
}

float audio_get_rms(void)
{
    size_t n = audio_read(rms_buf, AUDIO_BUF_SAMPLES);
    if (n == 0) return 0.0f;
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        double s = rms_buf[i] / 32768.0;
        sum += s * s;
    }
    return (float)sqrt(sum / n);
}

bool audio_spike_detected(float threshold_rms)
{
    return audio_get_rms() > threshold_rms;
}
