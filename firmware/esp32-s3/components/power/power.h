/**
 * power.h — AXP2101 power management IC driver
 *
 * The AXP2101 is an integrated PMU providing:
 *   - Battery charging (CC/CV + protection)
 *   - Multiple DC-DC converters and LDOs for peripherals
 *   - Battery voltage / current / temperature ADC
 *   - Coulomb counter for accurate state-of-charge estimation
 */
#pragma once

#include <stdbool.h>

/** Battery status snapshot */
typedef struct {
    float voltage_v;    /**< Battery voltage [V] */
    float current_ma;   /**< Charge/discharge current [mA] — negative = discharge */
    int   soc_pct;      /**< State-of-charge [0–100 %] */
    bool  charging;     /**< True when connected to charger */
} power_status_t;

/** Initialise the AXP2101 and configure power rails for the watch. */
void power_init(void);

/** Read current battery status. */
void power_get_status(power_status_t *out);

/** Enable / disable individual power rails by name. */
void power_set_rail(const char *rail, bool enabled);

/** Configure power consumption for a named operating mode. */
void power_set_mode(const char *mode);  /* "active" | "monitor" | "sleep" */
