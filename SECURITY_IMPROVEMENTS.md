# Security Improvements

This document tracks all security hardening applied to the Buddhas-Watch firmware.

---

## PR #6 — Hardening Applied (July 2026)

**PR:** [#6](https://github.com/ArcMike0430/Buddhas-Watch/pull/6)  
**Merge commit:** `7fff344`  
**Source file:** `firmware/esp32-s3/main/watch_os.cpp`

### 1. Thread-Safe Time Handling — `localtime_r()` replacing `localtime()`

| Item | Detail |
|------|--------|
| **File** | `watch_os.cpp` |
| **Risk** | `localtime()` returns a pointer to a static internal buffer shared across threads. On FreeRTOS SMP targets, concurrent calls from different tasks corrupt each other's `tm` structs silently. |
| **Fix** | Replaced all `localtime()` calls with `localtime_r(&t, &tm_buf)`, passing a stack-allocated `struct tm` to receive the result. |
| **Impact** | Eliminates potential data-race undefined behaviour on dual-core ESP32-S3. |

### 2. `unlink()` Error Checking for File Deletion

| Item | Detail |
|------|--------|
| **File** | `watch_os.cpp` |
| **Risk** | Silent `unlink()` failures leave stale data on the filesystem, which can cause settings to be read from an outdated file on the next boot. |
| **Fix** | The return value of `unlink()` is now checked; on failure an `ESP_LOGE` log entry is emitted with the path and `errno`. |
| **Impact** | Improves observability and prevents silent data persistence bugs. |

### 3. `fwrite()` Return Validation

| Item | Detail |
|------|--------|
| **File** | `watch_os.cpp` — `write_json_file()` helper |
| **Risk** | A partial write (e.g. due to SPIFFS full or SD-card error) would leave a truncated JSON file that fails to parse on the next boot, silently wiping all settings. |
| **Fix** | The return value of `fwrite()` is compared against `json_len`. On mismatch, `ESP_LOGE` is emitted. |
| **Impact** | Ensures settings files are always complete or the failure is reported immediately. |

### 4. Integer Overflow Protection in Time Calculations

| Item | Detail |
|------|--------|
| **File** | `watch_os.cpp` — DND (Do Not Disturb) scheduling |
| **Risk** | Adding user-supplied `uint16_t` values representing HH:MM could overflow when converting to minutes or seconds for comparison. |
| **Fix** | Intermediate calculations use `uint32_t` widening, and the operands are validated before arithmetic. |
| **Impact** | Prevents wrap-around bugs in DND window logic. |

### 5. Bounds Validation on `tm_hour` and `tm_min`

| Item | Detail |
|------|--------|
| **File** | `watch_os.cpp` |
| **Risk** | `localtime_r()` can return out-of-range values on corrupted RTC state or during DST edge cases. |
| **Fix** | `tm_hour` is clamped/validated to `[0, 23]` and `tm_min` to `[0, 59]` before use in comparisons. |
| **Impact** | Prevents DND logic from operating on garbage time values. |

### 6. No Credential Persistence in JSON

| Item | Detail |
|------|--------|
| **File** | `watch_os.cpp` — `save_unified_settings()` and `save_known_networks()` |
| **Risk** | Persisting Wi-Fi passwords or BLE pairing keys in plaintext JSON on the SPIFFS/SD filesystem exposes credentials to anyone with physical access or flash-read capability. |
| **Fix** | Only network SSID metadata is saved; `credentials_saved` is always written as `false`. Passwords are never written to the JSON file. |
| **Impact** | Eliminates plaintext credential storage; credentials must be re-entered or retrieved from the ESP-IDF NVS (which uses AES-128 encryption). |

---

## Security Scan Results

| Scan | Result | Date |
|------|--------|------|
| **CodeQL** | ✅ 0 alerts | July 2026 (PR #6) |
| **Secret scanning** | ✅ No secrets detected | July 2026 (PR #6) |
| **Code review** | ✅ Findings addressed | July 2026 (PR #6) |

---

## Recommendations for Future Hardening

The following items are not yet implemented and are recommended for subsequent work:

1. **NVS-backed credential storage** — Store Wi-Fi credentials in the encrypted ESP-IDF NVS partition rather than relying on re-entry. Use `esp_wifi_set_config()` directly after reading from NVS.

2. **Settings file integrity check** — Add a SHA-256 or CRC32 footer to `/data/settings_config.json` so corrupted files are detected at parse time rather than causing silent defaults.

3. **Command-receiver authentication** — The UDP command channel on port 5501 currently accepts commands from any LAN source. Add HMAC-SHA256 signing to the JSON command envelope and validate on the device side.

4. **SPIFFS/FAT partition encryption** — Enable flash encryption (ESP-IDF `CONFIG_FLASH_ENCRYPTION_ENABLED`) to protect at-rest settings data and credentials stored in NVS from offline flash-dump attacks.

5. **Stack canaries on sensitive tasks** — Enable `CONFIG_STACK_CHECK_ALL` for `cmd_receiver_task` and any task handling external data to catch stack smashing early.

6. **TLS for TCP streaming** — The Wi-Fi TCP streaming path is currently plaintext. Add mbedTLS wrapping for streams sent to the Jetson host to prevent MITM interception of CSI data.
