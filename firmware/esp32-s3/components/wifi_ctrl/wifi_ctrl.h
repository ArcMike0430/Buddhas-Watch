/**
 * wifi_ctrl.h — Wi-Fi channel locking, RF noise burst, vibrator
 *
 * Counter-measure functions called by cmd_receiver.c in response
 * to commands received from the Jetson monitor.
 */
#pragma once

#include <stdbool.h>

/**
 * Lock the Wi-Fi radio to a specific 2.4 GHz channel (1–13).
 * Prevents forced channel-switch (deauth) attacks.
 * Pass channel = 0 to restore dynamic channel selection.
 */
void wifi_ctrl_lock_channel(int channel);

/**
 * Transmit a brief RF noise burst on the given frequency band.
 *
 * The ESP32 enters a raw transmit mode and sends a pseudo-random
 * bit sequence, disrupting coherent interference in the RF field.
 *
 * @param freq_hz      Target frequency in Hz (mapped to nearest Wi-Fi channel)
 * @param duration_ms  Burst duration in milliseconds
 */
void wifi_ctrl_transmit_noise(float freq_hz, int duration_ms);

/** Cancel any ongoing noise transmission and restore normal operation. */
void wifi_ctrl_cancel_noise(void);

/** Pulse the vibration motor (delegated from display component). */
void wifi_ctrl_vibrate(int duration_ms);
