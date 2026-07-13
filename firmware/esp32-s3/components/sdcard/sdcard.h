/**
 * sdcard.h — SD card logging via SPI + FatFS
 *
 * The watch writes CSI event markers to /sdcard/log.jsonl for
 * offline analysis. Dual-mode: when UDP is unavailable, all
 * detection events are written here instead.
 */
#pragma once

#include <stdbool.h>

/** Mount the SD card. Returns true on success. */
bool sdcard_init(void);

/**
 * Write a JSON-lines marker to /sdcard/log.jsonl.
 *
 * @param event       Short event name, e.g. "anomaly_detected"
 * @param json_detail JSON object string with additional fields
 *
 * Example line written:
 *   {"ts":1720873265,"event":"anomaly_detected","freq":40000,"snr":22.5}
 */
void sdcard_write_log_marker(const char *event, const char *json_detail);

/** Flush and unmount the SD card (call before deep sleep). */
void sdcard_deinit(void);
