# Calibration — Buddhas-Watch

## Overview

Calibration establishes the quiet-environment baseline for each detection
algorithm. It must be run once per deployment location before the system
is used for detection. Re-calibrate whenever the environment changes
significantly (moved to a new location, major RF changes nearby).

## Step 1 — Hardware preparation

1. Power on all ESP32-S3 watches. Verify Wi-Fi connectivity (display shows
   `"Wi-Fi OK"` then `"Running"`).
2. Ensure the Jetson / Steam Deck is on the same Wi-Fi network.
3. Confirm watches are streaming: `nc -u -l 5500` and verify JSON packets
   appear on the Jetson.

## Step 2 — Run baseline calibrator

```bash
cd python/tools

# 2-minute baseline, all nodes (recommended minimum)
python baseline_calibrator.py --duration 120 --output baseline.json

# Extended 5-minute calibration for production deployments
python baseline_calibrator.py --duration 300 --output baseline.json
```

During calibration the subject should remain still and the environment
should be representative of the monitoring baseline (no known interference
sources active).

### Output: `baseline.json`

```json
{
  "timestamp": "2024-07-13T09:00:00.000000",
  "sampling_rate_hz": 1000.0,
  "nodes": {
    "watch_left": {
      "sample_count": 120000,
      "phase_variance": {
        "mean": 0.000412,
        "std":  0.000038,
        "p95":  0.000471,
        "p99":  0.000512
      },
      "fft_noise_floor":    1.23e-08,
      "spectral_coherence": 0.72,
      "recommended_threshold_sigma": 3.0,
      "recommended_persistence_frames": 5
    }
  }
}
```

## Step 3 — Visualise baseline

```bash
python baseline_calibrator.py --visualize baseline.json
```

Review the output. The `p95` variance should be well below the noise in your
target detection scenario. If it is unexpectedly high, check for persistent
background RF sources.

## Step 4 — Fleet sync

Synchronise all watch clocks to ensure cross-node timestamps are aligned:

```bash
python tools/fleet_broadcaster.py sync
```

## Step 5 — Start monitoring

```bash
# Full defence system (detection + anti-phase emission)
python csi_monitor/csi_defense.py

# Or spectrogram monitor with watch alerts
python csi_monitor/csi_spectrogram_monitor.py

# Or quantum-enhanced monitor
python -c "
from quantum.quantum_enhanced_detection import create_detector
det = create_detector()
# ... integrate into your UDP listener
"
```

## Recalibration triggers

Recalibrate when:
- False-alarm rate increases significantly
- The system is moved to a new location
- Major RF equipment changes nearby (new AP, microwave, etc.)
- More than 7 days since last calibration in an unstable RF environment

## VariationalAnomalyDetector training

For the VQE-based detector, provide labelled examples:

```python
from quantum.variational_anomaly import VariationalAnomalyDetector
import numpy as np

# Collect 20 normal frames and 20 known anomaly frames
normal_frames  = [...]   # list of np.ndarray shape (52,)
anomaly_frames = [...]

vad = VariationalAnomalyDetector(n_qubits=4, n_iters=50)
vad.fit(normal_frames, anomaly_frames)

# Save threshold for later use
import json
with open('vad_threshold.json', 'w') as f:
    json.dump({'threshold': vad.threshold}, f)
```
