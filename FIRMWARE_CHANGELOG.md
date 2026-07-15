# Firmware Changelog

## v2.0.0 — WatchOS Scaffold (PR #6)

**Merged:** July 15, 2026  
**PR:** [#6 — Refactor ESP32-S3 firmware into WatchOS scaffold with launcher, settings modules, and multi-protocol streaming orchestration](https://github.com/ArcMike0430/Buddhas-Watch/pull/6)  
**Merge commit:** `7fff344`  
**Changes:** 863 additions, 129 deletions across 8 files  

---

### Overview

This release pivots the firmware from a single-purpose CSI node to a full OS-style structure
aligned with the target watch architecture. The monolithic `main.c` entry point has been
replaced with a `main.cpp` + `WatchOS` boot orchestration layer that sequences all subsystems
at startup.

---

### Boot / Runtime Architecture

- **Removed** `firmware/esp32-s3/main/main.c` (monolithic entry point, 125 lines)
- **Added** `firmware/esp32-s3/main/main.cpp` — thin C++ entry; calls `WatchOS::boot()`
- **Added** `firmware/esp32-s3/main/watch_os.cpp` + `watch_os.hpp` — OS orchestration layer
- Boot sequence:
  1. Launcher rendered
  2. Settings hub rendered and connectivity settings launched
  3. All settings persisted to `/data/settings_config.json`
  4. Multi-protocol streaming started
  5. UDP command receiver task started

```cpp
esp_err_t WatchOS::boot() {
    launcher_.render();
    settings_.render();
    settings_.launch_connectivity_settings();
    settings_.persist_all();      // writes /data/settings_config.json
    streaming_.start_all();       // BLE + TCP/UDP + USB CDC + mDNS
    start_cmd_receiver();         // UDP command path
    return ESP_OK;
}
```

---

### Settings App Foundation — 7 Modules

New structured C++ settings module classes under `buddhas_watch` namespace (`watch_os.hpp`):

| Module | Class | Key Functions |
|--------|-------|---------------|
| Connectivity | `ConnectivitySettings` | Wi-Fi list, BLE toggle, known network save |
| Display | `DisplaySettings` | Brightness, screen timeout, theme, AOD |
| Audio / Haptics | `AudioHapticsSettings` | Ringtone/media volume, vibration intensity |
| Power | `PowerSettings` | Sleep timeout, deep-sleep enable |
| Sensors / Health | `SensorsHealthSettings` | Microphone enable, DND scheduling |
| System / Storage | `SystemStorageSettings` | Diagnostics, factory reset, firmware version |
| Security / Privacy | `SecurityPrivacySettings` | Privacy mode toggle |

All settings are aggregated by `SettingsHub` and persisted via a unified JSON writer.

---

### Settings Persistence

- Settings file: `/data/settings_config.json`
- Known-network metadata: `/data/known_networks.json`
- Credentials are **not** stored in JSON (security hardening — see `SECURITY_IMPROVEMENTS.md`)
- The `/data` directory is created automatically if it does not exist

---

### App Launcher

`AppLauncher` class (`watch_os.hpp`) exposes a built-in app inventory:

1. CSI Collector
2. Settings
3. System Monitor
4. BLE Server
5. USB Server
6. Wi-Fi Server
7. App Store

---

### Multi-Protocol Streaming Orchestration

`StreamingManager` class with coordinated `start_all()` hook:

| Protocol | Transport |
|----------|-----------|
| BLE | Bluetooth Low Energy (GATT) |
| Wi-Fi TCP | TCP unicast to Jetson host |
| Wi-Fi UDP | UDP broadcast (LAN) |
| USB-C CDC | USB serial/CDC |
| mDNS | Service discovery publication |

---

### Command Receiver Hardening

`firmware/esp32-s3/main/cmd_receiver.c`:

- Explicit `cJSON_IsString()` validation before using command string
- NULL guard on `cJSON_GetStringValue()` return value
- `details_str` heap buffer properly freed after SD card write
- Malformed JSON logged and discarded cleanly without crashing

---

### Hardware Stubs

- **Added** `firmware/esp32-s3/main/hardware_stubs.c` — stub implementations of
  `display_show_alert`, `vibrator_pulse`, `led_flash_pattern`, `wifi_transmit_noise`,
  `wifi_lock_channel`, and `sdcard_write_log_marker` to allow firmware to compile without
  full peripheral drivers wired up.

---

### Build Graph Updates

`firmware/esp32-s3/CMakeLists.txt`:

- Added `main.cpp`, `watch_os.cpp`, `cmd_receiver.c`, `hardware_stubs.c` to `SRCS`
- Added IDF components: `nvs_flash`, `spiffs`, `fatfs`, `wear_levelling`, `esp_http_server`,
  `bt`, `mdns`, `usb`, `driver`, `json`
- Removed `main.c` from source list

---

## v1.x — CSI Monitor Baseline

Previous firmware consisted of a monolithic `main.c` focused on CSI collection,
quantum backend integration, and UDP streaming to Jetson host. See earlier commits for details.
