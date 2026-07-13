# Architecture — Buddhas-Watch

## System Overview

```
┌────────────────────────────────────────────────────┐
│                   WEARABLE LAYER                   │
│                                                    │
│  Watch A (Left Wrist)       Watch B (Right Wrist)  │
│  ┌────────────────────┐     ┌────────────────────┐ │
│  │ ESP32-S3            │     │ ESP32-S3            │ │
│  │  CSI (52 sub-carr.) │     │  CSI (52 sub-carr.) │ │
│  │  IMU  QMI8658       │     │  IMU  QMI8658       │ │
│  │  Mic  ES7210 x2     │     │  Mic  ES7210 x2     │ │
│  │  AMOLED 410×502     │     │  AMOLED 410×502     │ │
│  │  SD card (FatFS)    │     │  SD card (FatFS)    │ │
│  │  BLE (aux channel)  │     │  BLE (aux channel)  │ │
│  │  AXP2101 PMU        │     │  AXP2101 PMU        │ │
│  └────────┬───────────┘     └────────┬───────────┘ │
│           │ UDP:5500                 │ UDP:5500     │
└───────────┼─────────────────────────┼──────────────┘
            │                         │
            └──────────┬──────────────┘
                       │ Wi-Fi 2.4 GHz
                       ▼
┌────────────────────────────────────────────────────┐
│                  COMPUTE LAYER                     │
│                                                    │
│           Jetson Orin Nano / Steam Deck            │
│  ┌──────────────────────────────────────────────┐  │
│  │ python/csi_monitor/                          │  │
│  │   csi_phase_variance_monitor.py   ─────────┐ │  │
│  │   csi_phase_coherence_monitor.py  ──────── │ │  │
│  │   csi_spectrogram_monitor.py      ──────── │ │  │
│  │   csi_defense.py (integrated)     ──────── │ │  │
│  │                                            │ │  │
│  │ python/quantum/                            │ │  │
│  │   quantum_enhanced_detection.py (Qiskit)─── │ │  │
│  │   variational_anomaly.py (VQE learner)──── │ │  │
│  │                                            │ │  │
│  │ python/countermeasures/esp32_alert.py ◄────┘ │  │
│  │   └── UDP:5501 ─► ESP32 watches              │  │
│  └──────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────┘
```

## Data Flow

### Inbound (CSI acquisition)
1. The ESP32-S3 Wi-Fi radio captures raw CSI on each received packet.
2. The CSI callback (`wifi_csi_cb`) extracts the phase angle of each of the
   52 OFDM subcarriers using `atan2f(imag, real)`.
3. A JSON packet `{"node_id":..., "timestamp":..., "phases":[52 values]}` is
   sent via UDP to port 5500 on the Jetson IP.

### Detection pipeline
```
UDP RX → phase_buffers (per node)
       → PhaseVarianceMonitor (z-score vs baseline, 3σ threshold)
       → PhaseCoherenceMonitor (cross-subcarrier coherence, circular mean)
       → SpectrogramMonitor (FFT, narrowband peaks, persistence gate)
       → QuantumCoherenceDetector (Ry circuit + entropy coherence)
       → [anomaly] → WatchCommander.alert() / rf_burst() / log_marker()
```

### Outbound (counter-measures)
Commands flow from Jetson → UDP:5501 → ESP32 `cmd_receiver_task`.

| Command       | Firmware action                              |
|---------------|----------------------------------------------|
| `alert`       | `display_show_alert()` + vibrator pulse      |
| `rf_burst`    | `wifi_ctrl_transmit_noise()` (raw 802.11 TX) |
| `lock`        | `wifi_ctrl_lock_channel()`                   |
| `log_marker`  | `sdcard_write_log_marker()`                  |
| `sweep`       | Step channels ch_start→ch_end                |
| `silence`     | `display_show_alert("none")` + cancel noise  |
| `sync`        | Log UTC epoch (RTC update TODO)              |
| `calibrate`   | Mark calibration window on SD card           |
| `mode`        | Switch operating mode + log                  |
| `shutdown`    | `esp_deep_sleep_start()`                     |

## Firmware Component Graph

```
main.c ──► csi/         wifi_csi_cb registers CSI capture
       ──► imu/         QMI8658 motion detection
       ──► audio/       ES7210 dual-mic RMS monitoring
       ──► display/     AMOLED + LED + vibrator
       ──► sdcard/      FatFS JSONL logging
       ──► ble/         BLE GATT notification channel
       ──► wifi_ctrl/   channel lock + RF noise burst
       ──► power/       AXP2101 battery + rail control
cmd_receiver.c ──► display/ sdcard/ wifi_ctrl/
```

## Multi-Modal Fusion

| Sensor    | CSI spike present | Decision     |
|-----------|-------------------|--------------|
| IMU       | Motion detected   | Body movement — low priority |
| IMU       | No motion         | RF-acoustic event — high priority |
| Audio     | Acoustic spike    | Acoustoelectric coupling confirmed |
| Audio     | Silent            | Pure RF anomaly |
| Dual node | Both nodes spike  | Real event (high confidence) |
| Dual node | Single node spike | Local noise or EMI |
