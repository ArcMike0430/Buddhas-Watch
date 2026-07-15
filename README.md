# 🧿 Buddhas-Watch

**Production-grade distributed CSI collection system for ESP32-S3 smart watches**

A real-time, multi-node Channel State Information (CSI) monitoring and anomaly detection system that transforms wearable ESP32-S3 smart watches into a distributed quantum-augmented sensing network for RF-acoustic biometrics, interference detection, and Human Quantum Interface (HQI) research.

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

### Firmware

The ESP32-S3 firmware includes `cmd_receiver.c` which listens on UDP port 5501 and dispatches commands to hardware control functions. Implement the hardware functions (`display_show_alert`, `vibrator_pulse`, `wifi_transmit_noise`, etc.) to match your specific watch hardware.

The firmware boot path now starts from `main/main.cpp` and initializes a full watch OS scaffold (`main/watch_os.cpp`) that includes:
- Touch launcher app list (CSI Collector, Settings, System Monitor, BLE/USB/Wi-Fi servers, App Store)
- Settings app with 7 modules (Connectivity, Display, Audio, Power, Sensors, System, Security)
- Multi-protocol CSI streaming manager (BLE, Wi-Fi TCP/UDP, USB-C CDC, mDNS)
- Unified settings persistence in `/data/settings_config.json`

### Data Logging
- SD card local logging (offline mode — operates without Jetson)
- UDP streaming to Jetson (online mode)
- Dual-mode operation — seamless transition

## Repository Structure

```
Buddhas-Watch/
├── firmware/
│   └── esp32-s3/
│       ├── main/                  # Main firmware entry point
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
│   │   ├── csi_phase_coherence_monitor.py     # + Cross-subcarrier coherence
│   │   ├── csi_spectrogram_monitor.py         # FFT spectrogram + persistence + watch alerts
│   │   └── csi_defense.py                    # Integrated detection + anti-phase
│   ├── quantum/
│   │   ├── quantum_enhanced_detection.py      # Qiskit hybrid backend + classical fallback
│   │   └── variational_anomaly.py             # (future) VQE-based anomaly threshold
│   ├── analysis/
│   │   └── (analysis tools — coming soon)
│   ├── countermeasures/
│   │   └── esp32_alert.py                    # UDP commands back to watches (alert, rf_burst, lock, sweep)
│   └── tools/
│       ├── baseline_calibrator.py             # Baseline learning utility → baseline.json
│       └── fleet_broadcaster.py              # Multi-node sync/command broadcast
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

The fundamental detection principle: modulated ultrasound (from photoacoustic, parametric speaker, or other sources) changes tissue conductivity. This modulates any RF carrier passing through that tissue. CSI extracts the phase/amplitude sidebands — turning the body into an RF-acoustic transducer.

### BitNet Ternary Architecture

All weights are ternary (-1, 0, +1). This collapses 32-bit precision to 2-bit states, making inference massively parallel, power-efficient, and directly mappable to hardware registers. The same operations that execute CSI feature extraction can manipulate efuse registers for hardware security — because at the physical layer, both are just deciding which bits are 0 and which are 1.

### Human Quantum Interface (HQI)

The system targets detection of quantum biology phenomena — microtubule Fröhlich condensation, radical-pair spin chemistry, pineal quantum coherence, hydrated matrix polaritons — through their CSI signatures. The ternary + quantum hybrid pipeline provides the computational speed and sensitivity needed to detect these at physiological temperatures.

## License

MIT — see [LICENSE](LICENSE)

## Author

**Michael L. Mejia** — ArcMike0430
Eteru AI Systems / HQI Research
