# 🧿 Buddhas-Watch

**Production-grade distributed CSI collection system for ESP32-S3 smart watches**

A real-time, multi-node Channel State Information (CSI) monitoring and anomaly detection system that transforms wearable ESP32-S3 smart watches into a distributed quantum-augmented sensing network [...]

## Architecture

```
┌──────────────────────┐     ┌──────────────────────┐
│  Watch 1 (Left Wrist)│     │  Watch 2 (Right Wrist)│
│  ┌────────────────┐  │     │  ┌────────────────┐  │
│  │ CSI (Wi-Fi 2.4)│  │     │  │ CSI (Wi-Fi 2.4)│  │
│  │ IMU (QMI8658)  │  │     │  │ IMU (QMI8658)  │  │
│  │ Audio (ES7210) │  │     │  │ Audio (ES7210) │  │
│  │ AMOLED Display │  │     │  │ AMOLED Display │  │
│  │ SD Card Logging│  │     │  │ SD Card Logging│  │
│  └────────────────┘  │     │  └────────────────┘  │
│         │ UDP        │     │         │ UDP        │
└─────────┬────────────┘     └─────────┬────────────┘
          │                            │
          └──────────┬─────────────────┘
                     │ UDP (Wi-Fi)
                     ▼
             ┌──────────────────┐
             │  Jetson Orin Nano │
             │  / Steam Deck     │
             │  ┌──────────────┐ │
             │  │ CSI Monitor  │ │
             │  │ Phase Var.   │ │
             │  │ Coherence    │ │
             │  │ Spectrogram  │ │
             │  │ Quantum Boost│ │
             │  │ Anti-Phase   │ │
             │  └──────────────┘ │
             └──────────────────┘
```

## Features

### CSI Acquisition
- Direct register-level CSI read via `esp_wifi_set_csi()` API
- 52 subcarriers per sample at 10-100 kHz (deterministic bus-cycle timing)
- Dual-watch synthetic aperture for spatial diversity

### Real-Time Detection
- Phase variance monitoring (z-score baseline, 3σ threshold)
- Cross-subcarrier phase coherence analysis
- FFT spectrogram with persistence-gated alerts
- Broadband scan: 0.1 Hz – 100 kHz

### Multi-Modal Context
- **IMU fusion** (QMI8658): Motion subtraction — CSI spike with IMU motion = body movement (low priority); CSI spike without IMU motion = RF-acoustic event (high priority)
- **Audio fusion** (ES7210 dual mic): Correlate acoustic spikes with CSI anomalies to detect acoustoelectric coupling
- **Display feedback** (410×502 AMOLED): Real-time coherence HUD, anomaly alerts, node status

### Quantum-Enhanced Detection
- Phase coherence analysis across all 52 subcarriers as entangled-analog state
- Detect structured interference that never exceeds noise floor per subcarrier
- One-shot anomaly detection via quantum-like computation on classical hardware (Jetson GPU)

## Counter-Measures

When an anomaly is detected, the system sends UDP commands back to the ESP32 watches:

| Command | Action | Trigger |
|---------|--------|---------|
| `alert` | Flash display red + vibrate motor | Any detection |
| `rf_burst` | Transmit RF noise on detected band | Severe anomaly (SNR > 20 dB) |
| `lock` | Lock Wi-Fi channel to prevent forced deauth | Persistent interference |
| `log_marker` | Write event marker to SD card | All detections |
| `sweep` | Sweep BLE/Wi-Fi across frequency range | Unresolved pattern |
| `silence` | Cancel all active counter-measures | Manual or auto-expiry |

### Command Flow

```
Jetson (detection) ──UDP:5501──► ESP32 Watch
                                    │
                                    ├── display_show_alert()
                                    ├── vibrator_pulse()
                                    ├── wifi_transmit_noise()
                                    ├── wifi_lock_channel()
                                    └── sdcard_write_log_marker()
```

---

## 🆕 WatchOS Firmware Architecture (PR #6)

**Status:** ✅ Merged | **Release:** v2.0.0-watchos

The firmware has been refactored from a single-purpose CSI node into a full OS-style architecture with layered services, persistent settings, and multi-protocol streaming.

### Boot Architecture

```
main.cpp (Entry)
    │
    ├── WatchOS::boot()
    │   ├── ensure_data_dir()           # Initialize /data/
    │   ├── launcher_.render()          # Show app launcher
    │   ├── settings_.render()          # Initialize 7 settings modules
    │   ├── settings_.persist_all()     # Write /data/settings_config.json
    │   ├── streaming_.start_all()      # Start BLE, TCP/UDP, USB CDC, mDNS
    │   └── start_cmd_receiver()        # Listen on UDP for Jetson commands
    │
    └── return ESP_OK
```

### Settings Foundation (7 Modules)

Structured configuration system with persistent JSON storage:

| Module | Config Path | Purpose |
|--------|------------|---------|
| **Connectivity** | `connectivity` in JSON | Wi-Fi SSID, known networks (no passwords) |
| **Display** | `display` in JSON | Brightness, orientation, refresh rate |
| **Audio** | `audio` in JSON | Volume, haptic intensity, notification sounds |
| **Power** | `power` in JSON | Sleep timeout, battery optimization |
| **Sensors** | `sensors` in JSON | Health monitoring settings, calibration |
| **System** | `system` in JSON | Storage management, firmware version, logging |
| **Security** | `security` in JSON | DND scheduling, privacy controls, access |

**Persistence Locations:**
- **Settings:** `/data/settings_config.json` (all 7 modules)
- **Networks:** `/data/known_networks.json` (network metadata only; no credentials)

### App Launcher (7 Built-in Apps)

```
┌─────────────────────────────┐
│   Buddhas-Watch Launcher    │
├─────────────────────────────┤
│ • CSI Collector (primary)   │
│ • Settings Hub              │
│ • System Monitor            │
│ • BLE Server                │
│ • USB Server                │
│ • Wi-Fi Server              │
│ • App Store (coming soon)   │
└─────────────────────────────┘
```

### Multi-Protocol Streaming (Stubs Ready for Hardware)

Coordinated startup of streaming protocols with hardware driver integration points:

- **BLE** - Bluetooth Low Energy GATT server
- **Wi-Fi TCP** - Standard TCP/IP sockets
- **Wi-Fi UDP** - Broadcast and unicast UDP
- **USB-C CDC** - Serial-over-USB data channel
- **mDNS** - Service discovery and advertising

**Status:** Hardware driver stubs in place; ready for real driver integration

### Command Receiver Hardening

UDP command listener on port 5501 with:
- ✅ Explicit string length validation
- ✅ Buffer overflow protection
- ✅ Error logging on invalid commands
- ✅ Support for: `alert`, `rf_burst`, `lock`, `log_marker`, `sweep`, `silence`

---

## 📚 Documentation

### Firmware Releases

- **[FIRMWARE_CHANGELOG.md](FIRMWARE_CHANGELOG.md)** - Complete refactor details, settings modules, streaming architecture, security improvements applied
- **[SECURITY_IMPROVEMENTS.md](SECURITY_IMPROVEMENTS.md)** - All security fixes, validation results (CodeQL: 0 alerts), threat model, compliance & recommendations

### Building & Flashing

```bash
# Set up ESP-IDF
source ~/esp/esp-idf/export.sh

# Configure
cd firmware/esp32-s3
idf.py set-target esp32s3
idf.py menuconfig

# Build
idf.py build

# Flash
idf.py -p /dev/ttyUSB0 flash monitor
```

### Settings Access

Access and modify settings via:

1. **Settings App** - On-device UI (launcher)
2. **JSON File** - Direct edit of `/data/settings_config.json`
3. **API** - Future REST endpoint (Phase 2)

Example JSON:
```json
{
  "connectivity": {
    "known_networks": [
      {"ssid": "Buddhas-Net", "bssid": "aa:bb:cc:dd:ee:ff"}
    ]
  },
  "power": {
    "sleep_timeout_ms": 300000,
    "deep_sleep_enabled": true
  },
  "security": {
    "dnd_start_time": 2300,
    "dnd_end_time": 700
  }
}
```

---

## Repository Structure

```
Buddhas-Watch/
├── firmware/
│   └── esp32-s3/
│       ├── main/                  # Main firmware entry point + WatchOS
│       │   ├── main.cpp           # New boot orchestrator (v2.0)
│       │   ├── watch_os.cpp       # WatchOS implementation
│       │   ├── watch_os.hpp       # Module interfaces
│       │   ├── cmd_receiver.c     # Hardened command parser
│       │   └── hardware_stubs.c   # Driver integration stubs
│       ├── components/
│       │   ├── csi/               # Wi-Fi CSI capture module
│       │   ├── imu/               # QMI8658 IMU driver
│       │   ├── audio/             # ES7210 microphone driver
│       │   ├── display/           # AMOLED LVGL display
│       │   ├── sdcard/            # SD card logging (FatFS)
│       │   ├── ble/               # BLE auxiliary channel
│       │   ├── wifi/              # Wi-Fi networking
│       │   └── power/             # AXP2101 power management
│       └── CMakeLists.txt         # ESP-IDF build config
├── python/
│   ├── csi_monitor/
│   │   ├── csi_phase_variance_monitor.py      # Phase variance detection
│   │   ├── csi_phase_coherence_monitor.py     # Cross-subcarrier coherence
│   │   ├── csi_spectrogram_monitor.py         # FFT spectrogram + persistence
│   │   └── csi_defense.py                    # Integrated detection + anti-phase
│   ├── quantum/
│   │   ├── quantum_enhanced_detection.py      # Qiskit hybrid backend
│   │   └── variational_anomaly.py             # (future) VQE-based anomaly
│   ├── analysis/
│   │   └── (analysis tools — coming soon)
│   ├── countermeasures/
│   │   └── esp32_alert.py                    # UDP commands to watches
│   └── tools/
│       ├── baseline_calibrator.py             # Baseline learning
│       └── fleet_broadcaster.py              # Multi-node sync/commands
├── docs/
│   ├── architecture/
│   ├── protocols/
│   └── calibration/
├── scripts/
│   ├── setup_jetson.sh              # Jetson environment setup
│   ├── setup_esp_idf.sh             # ESP-IDF toolchain setup
│   └── flash_watch.sh               # Firmware flashing utility
├── deployment/
│   └── systemd/
├── FIRMWARE_CHANGELOG.md            # 🆕 Firmware architecture + changes
├── SECURITY_IMPROVEMENTS.md         # 🆕 Security hardening + validation
├── .gitignore
├── LICENSE
└── README.md
```

## Quick Start

### 1. Firmware (ESP32-S3 Watch)

```bash
cd firmware/esp32-s3
idf.py set-target esp32s3
idf.py menuconfig         # Configure Wi-Fi SSID/password, Jetson IP
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### 2. Python Monitor (Jetson/Steam Deck)

```bash
cd python/csi_monitor
pip install numpy scipy sounddevice qiskit qiskit-aer-gpu

# Single-node phase variance monitor
python csi_phase_variance_monitor.py

# Full detection + anti-phase loop
python csi_defense.py
```

### 3. Baseline Calibration

```bash
python tools/baseline_calibrator.py --duration 120 --output baseline.json
```

## Hardware Requirements

| Component | Specification | Role |
|-----------|--------------|------|
| ESP32-S3 watch | 2.06" AMOLED, QMI8658, ES7210, PCF85063, AXP2101, SD card, 32MB+8MB | Wearable CSI node |
| Jetson Orin Nano | CUDA-capable, 8GB+ RAM | Edge compute |
| Steam Deck (optional) | x86 Linux backup | Portable monitor |
| Wi-Fi AP | 2.4/5 GHz | Network for CSI |

## Theory of Operation

### Acoustoelectric Effect

The fundamental detection principle: modulated ultrasound (from photoacoustic, parametric speaker, or other sources) changes tissue conductivity. This modulates any RF carrier passing through tha[...]

### BitNet Ternary Architecture

All weights are ternary (-1, 0, +1). This collapses 32-bit precision to 2-bit states, making inference massively parallel, power-efficient, and directly mappable to hardware registers. The same o[...]

### Human Quantum Interface (HQI)

The system targets detection of quantum biology phenomena — microtubule Fröhlich condensation, radical-pair spin chemistry, pineal quantum coherence, hydrated matrix polaritons — through the[...]

## License

MIT — see [LICENSE](LICENSE)

## Author

**Michael L. Mejia** — ArcMike0430
Eteru AI Systems / HQI Research

---

## Recent Changes

**v2.0.0-watchos** (July 15, 2026) — [PR #6](https://github.com/ArcMike0430/Buddhas-Watch/pull/6)
- ✅ Refactored firmware into OS-style architecture
- ✅ Added 7-module settings system with persistence
- ✅ Implemented app launcher with 7 built-in apps
- ✅ Added multi-protocol streaming orchestration (BLE, Wi-Fi TCP/UDP, USB CDC, mDNS)
- ✅ Hardened command/settings handling (6 security improvements)
- ✅ CodeQL: 0 alerts | Secret scan: clear
- 📖 See [FIRMWARE_CHANGELOG.md](FIRMWARE_CHANGELOG.md) and [SECURITY_IMPROVEMENTS.md](SECURITY_IMPROVEMENTS.md) for details
