/**
 * csi.h — Wi-Fi CSI capture helper
 */
#pragma once

#include <stdbool.h>
#include "esp_wifi.h"

/** Initialise CSI capture with default config and register a callback. */
void csi_init(wifi_csi_cb_t callback, void *ctx);

/** Enable / disable CSI capture at runtime. */
void csi_set_enabled(bool enabled);
