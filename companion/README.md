# Buddhas-Watch Companion Apps

Multi-platform companion applications for the Buddhas-Watch ESP32-S3 smartwatch.

## Android (Google Play Store)

### Build

```bash
cd companion/android
./gradlew assembleRelease
```

The release APK is generated at `app/build/outputs/apk/release/app-release.apk`.

### Features
- BLE scan for nearby Buddhas-Watch devices (advertises service UUID `0xAB00`)
- Live CSI magnitude/phase chart (MPAndroidChart)
- WiFi UDP mode — receive JSON-Lines on port 5500
- Background foreground service for continuous monitoring
- Export data to JSON-Lines

### Distribution
Google Play Store — upload the signed release APK via Play Console.

---

## Desktop Companion (Windows / macOS / Linux / Steam Deck)

### Install

```bash
cd companion/desktop
pip install -r requirements.txt
python csi_companion.py
```

### Options

```
python csi_companion.py --help

  --udp-port N   UDP port to listen on (default 5500)
  --ble          Auto-scan BLE on startup
  --no-gui       Headless mode (stdout + optional --output file)
  --output FILE  JSON-Lines output file (headless mode)
```

### Build standalone executable

```bash
pip install pyinstaller
pyinstaller --onefile --windowed csi_companion.py
# Output: dist/csi_companion(.exe)
```

### Steam Deck

1. Switch to Desktop Mode
2. Install Python 3.11+: `flatpak install flathub org.freedesktop.Platform`
3. Run: `python csi_companion.py`

Or use the pre-built AppImage from GitHub Releases.

### Features
- WiFi UDP listener (real-time, multi-node)
- BLE connection via `bleak` (all platforms)
- Live matplotlib charts (magnitude + phase per subcarrier)
- Record / stop / export (JSON-Lines, CSV, HDF5)
- Headless mode for Jetson / server use

---

## Jetson Orin Nano Integration

The existing Python monitors in `python/csi_monitor/` already listen on UDP :5500.
Run them alongside the desktop companion or independently:

```bash
# Phase variance monitor
python python/csi_monitor/csi_phase_variance_monitor.py

# Coherence monitor
python python/csi_monitor/csi_phase_coherence_monitor.py

# Spectrogram with persistence gating
python python/csi_monitor/csi_spectrogram_monitor.py
```

All monitors receive CSI from the watch's WiFi CSI app (UDP :5500 JSON stream).

---

## BLE Protocol Reference

| Characteristic | UUID | Properties | Description |
|---|---|---|---|
| CSI Metadata | `0xAB01` | Notify | Channel, RSSI, rate, timestamp, subcarrier count |
| CSI Data | `0xAB02` | Notify | Chunked magnitude+phase pairs (uint8 encoded) |
| Control | `0xAB03` | Write | `0x01`=Start, `0x00`=Stop, `0x02`=Toggle SD log |

Service UUID: `0000AB00-0000-1000-8000-00805F9B34FB`

## WiFi UDP Protocol Reference

JSON-Lines format on UDP port 5500:
```json
{
  "node_id": "watch_01",
  "timestamp": 1234567890,
  "channel": 6,
  "rssi": -45,
  "rate": 54,
  "phases": [-3.14, ...],
  "magnitudes": [12.3, ...]
}
```
