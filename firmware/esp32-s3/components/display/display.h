/**
 * display.h — 410×502 AMOLED display + LED + vibrator motor
 *
 * The watch uses a 410×502 AMOLED panel driven over SPI.
 * This component also manages the status LED and vibration motor
 * since they are all part of the user-feedback subsystem.
 */
#pragma once

#include <stdint.h>

/** Initialise the AMOLED display, LED, and vibrator GPIO. */
void display_init(void);

/**
 * Show an alert overlay on the display.
 *
 * @param severity  "none" | "medium" | "high" | "error"
 * @param freq_hz   Detected frequency to annotate (0 = unknown)
 */
void display_show_alert(const char *severity, float freq_hz);

/**
 * Show a short status string in the status bar.
 * Safe to call from any task.
 */
void display_show_status(const char *text);

/**
 * Show the coherence HUD — a live bar-graph of coherence [0.0–1.0].
 */
void display_show_coherence(float coherence, float baseline);

/**
 * Flash the status LED in the given pattern.
 *
 * @param pattern  "solid" | "pulse" | "none"
 */
void display_led_flash(const char *pattern);

/**
 * Pulse the vibration motor for duration_ms milliseconds.
 */
void wifi_ctrl_vibrate(int duration_ms);
