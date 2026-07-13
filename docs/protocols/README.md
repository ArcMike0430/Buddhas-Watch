# Protocols — Buddhas-Watch

## UDP Packet Formats

### CSI Telemetry (ESP32 → Jetson, port 5500)

```json
{
  "node_id":           "watch_left",
  "timestamp":         1720873265123456,
  "rssi":              -62,
  "rate":              12,
  "sig_mode":          1,
  "channel":           6,
  "secondary_channel": 0,
  "phases": [
    -1.5708, 0.2341, 1.0472, ...
  ]
}
```

| Field               | Type    | Description                                |
|---------------------|---------|--------------------------------------------|
| `node_id`           | string  | Watch identifier (`watch_left` / `watch_right` / `pocket`) |
| `timestamp`         | integer | `esp_timer_get_time()` microseconds since boot |
| `rssi`              | integer | Received signal strength [dBm]             |
| `rate`              | integer | MCS rate index                             |
| `sig_mode`          | integer | 0 = non-HT, 1 = HT, 3 = VHT               |
| `channel`           | integer | Primary Wi-Fi channel (1–13)               |
| `secondary_channel` | integer | Secondary channel offset (HT40)            |
| `phases`            | array   | Float phase angles in radians [−π, π], one per subcarrier |

### Command (Jetson → ESP32, port 5501)

```json
{
  "cmd":       "alert",
  "params":    { "freq": 40000, "severity": "high", "duration_ms": 200 },
  "timestamp": 1720873265.123,
  "broadcast": false
}
```

| Field       | Type    | Description                                       |
|-------------|---------|---------------------------------------------------|
| `cmd`       | string  | Command name (see table below)                    |
| `params`    | object  | Command-specific parameters                       |
| `timestamp` | float   | Unix epoch seconds (Jetson clock)                 |
| `broadcast` | boolean | True when sent by FleetBroadcaster to all watches |

### Command Reference

| `cmd`        | Required params          | Optional params                         |
|--------------|--------------------------|-----------------------------------------|
| `alert`      | —                        | `freq`, `severity` (`"medium"` / `"high"`), `duration_ms` |
| `rf_burst`   | `freq`                   | `duration_ms` (default 100)             |
| `lock`       | `channel` (1–13, or 0)   | —                                       |
| `log_marker` | `event`                  | `details` (any JSON object)             |
| `sweep`      | `start_mhz`, `end_mhz`   | `step_hz`, `duration_ms`                |
| `silence`    | —                        | —                                       |
| `sync`       | `utc_epoch_s`            | `utc_iso`                               |
| `calibrate`  | —                        | `duration_s` (default 120)              |
| `mode`       | `mode`                   | (`"monitor"` / `"log"` / `"scan"` / `"defend"`) |
| `shutdown`   | —                        | —                                       |

## SD Card Log Format (JSONL)

Each line in `/sdcard/log.jsonl` is a JSON object:

```
{"ts":1720873265,"event":"boot","detail":{"node":"watch_left"}}
{"ts":1720873267,"event":"anomaly_detected","detail":{"freq":40000,"snr":22.5}}
{"ts":1720873270,"event":"calibrate_start","detail":{}}
{"ts":1720873280,"event":"mode_change","detail":{"mode":"defend"}}
```

## BLE GATT Profile

- **Service UUID**: `BDDA0001-0000-1000-8000-00805F9B34FB` (custom)
- **Notify Characteristic**: up to 20 bytes UTF-8 text notification
- **Device name**: matches `node_id` (e.g., `watch_left`)
- **Advertising interval**: 20–40 ms
- **Appearance**: 0x00C1 (Generic Watch)
