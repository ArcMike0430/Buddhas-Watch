/**
 * wifi_ctrl.c — Wi-Fi channel locking and RF noise counter-measure
 *
 * Channel locking: esp_wifi_set_channel() fixes the radio to a
 * single 2.4 GHz channel, preventing forced deauthentication.
 *
 * RF burst: the ESP32 enters a vendor-specific raw transmit mode
 * and broadcasts a short burst of pseudo-random data on the
 * detected interference channel to disrupt coherent sources.
 */

#include "wifi_ctrl.h"
#include <stdlib.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG  = "WIFI_CTRL";
static bool noise_active = false;
static int  locked_channel = 0;

/* ------------------------------------------------------------------ */
/*  Channel locking                                                     */
/* ------------------------------------------------------------------ */
void wifi_ctrl_lock_channel(int channel)
{
    if (channel < 1 || channel > 13) {
        if (channel == 0) {
            /* Restore automatic channel selection */
            ESP_LOGI(TAG, "Channel lock released");
            locked_channel = 0;
            return;
        }
        ESP_LOGW(TAG, "Invalid channel %d — must be 1-13", channel);
        return;
    }

    esp_err_t ret = esp_wifi_set_channel((uint8_t)channel, WIFI_SECOND_CHAN_NONE);
    if (ret == ESP_OK) {
        locked_channel = channel;
        ESP_LOGI(TAG, "Wi-Fi locked to channel %d (%.0f MHz)",
                 channel, 2407.0f + channel * 5.0f);
    } else {
        ESP_LOGW(TAG, "Failed to lock channel %d: %s", channel, esp_err_to_name(ret));
    }
}

/* ------------------------------------------------------------------ */
/*  RF noise burst                                                      */
/* ------------------------------------------------------------------ */

/* Map frequency in Hz to nearest 2.4 GHz Wi-Fi channel */
static int freq_hz_to_wifi_channel(float freq_hz)
{
    /* 2.4 GHz band: channel 1 = 2412 MHz … channel 13 = 2472 MHz */
    float freq_mhz = freq_hz / 1e6f;
    if (freq_mhz < 2412.0f) freq_mhz = 2412.0f;
    if (freq_mhz > 2472.0f) freq_mhz = 2472.0f;
    int ch = (int)((freq_mhz - 2412.0f) / 5.0f) + 1;
    if (ch < 1)  ch = 1;
    if (ch > 13) ch = 13;
    return ch;
}

void wifi_ctrl_transmit_noise(float freq_hz, int duration_ms)
{
    /* Map to a Wi-Fi channel and switch to it temporarily */
    int ch = freq_hz_to_wifi_channel(freq_hz);
    ESP_LOGI(TAG, "RF noise burst: %.0f Hz → ch%d, %d ms", freq_hz, ch, duration_ms);

    noise_active = true;

    /* Temporarily switch to the target channel */
    int prev_channel = locked_channel > 0 ? locked_channel : 6;
    esp_wifi_set_channel((uint8_t)ch, WIFI_SECOND_CHAN_NONE);

    /*
     * Vendor-specific raw TX via esp_wifi_80211_tx():
     * Build a minimal 802.11 probe-request frame carrying random payload
     * as a cheap noise source.  Frame = FC(2) + Dur(2) + DA(6) + SA(6)
     * + BSSID(6) + SeqCtrl(2) + payload(N).
     */
    uint8_t frame[64];
    memset(frame, 0, sizeof(frame));
    /* Frame Control: subtype 0x04 = Probe Request, type Management */
    frame[0] = 0x40; frame[1] = 0x00;
    /* Duration */
    frame[2] = 0xFF; frame[3] = 0xFF;
    /* DA: broadcast */
    memset(frame + 4,  0xFF, 6);
    /* Use esp_random() (hardware TRNG) for better unpredictability;
     * rand() can produce detectable patterns that RF filters could exploit. */
    for (int i = 10; i < 22; i++) frame[i] = (uint8_t)(esp_random() & 0xFF);
    /* Payload: random */
    for (int i = 24; i < 64; i++) frame[i] = (uint8_t)(esp_random() & 0xFF);

    int64_t end_us = esp_timer_get_time() + (int64_t)duration_ms * 1000LL;
    while (esp_timer_get_time() < end_us && noise_active) {
        /* esp_wifi_80211_tx() is available in ESP-IDF ≥ 4.x */
        esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), false);
        vTaskDelay(1);  /* yield every frame */
    }

    /* Restore previous channel */
    esp_wifi_set_channel((uint8_t)prev_channel, WIFI_SECOND_CHAN_NONE);
    noise_active = false;
    ESP_LOGI(TAG, "RF noise burst complete");
}

void wifi_ctrl_cancel_noise(void)
{
    noise_active = false;
    ESP_LOGI(TAG, "RF noise cancelled");
}
