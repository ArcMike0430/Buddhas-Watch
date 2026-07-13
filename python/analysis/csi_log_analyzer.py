#!/usr/bin/env python3
"""
csi_log_analyzer.py
Offline analysis of JSONL logs produced by the ESP32-S3 SD card logger.

Reads /sdcard/log.jsonl (or any JSONL file), extracts phase variance
timelines, plots spectrogram snapshots, and exports a detection summary.

Usage:
    python csi_log_analyzer.py log.jsonl
    python csi_log_analyzer.py log.jsonl --plot --output summary.json
"""

import argparse
import json
import sys
from collections import defaultdict
from datetime import datetime
from pathlib import Path
from typing import List, Dict, Optional

import numpy as np

try:
    import matplotlib.pyplot as plt
    MATPLOTLIB = True
except ImportError:
    MATPLOTLIB = False

try:
    from scipy import signal as scipy_signal
    SCIPY = True
except ImportError:
    SCIPY = False


# ------------------------------------------------------------------ #
#  Loader                                                              #
# ------------------------------------------------------------------ #

def load_jsonl(path: str) -> List[Dict]:
    """Load a JSONL file, skipping malformed lines."""
    records = []
    with open(path) as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError as e:
                print(f"  Warning: line {lineno} skipped ({e})", file=sys.stderr)
    return records


# ------------------------------------------------------------------ #
#  Timeline extraction                                                 #
# ------------------------------------------------------------------ #

def extract_phase_timeline(records: List[Dict],
                            node_id: Optional[str] = None) -> Dict[str, np.ndarray]:
    """
    Extract per-node phase timelines from CSI packet records.

    Supports two record formats:
      - SD-log format: {"ts": <s>, "event": "csi", "detail": {"node_id": ..., "phases": [...]}}
      - UDP-capture format: {"node_id": ..., "timestamp": <µs>, "phases": [...]}
    """
    timelines: Dict[str, list] = defaultdict(list)

    for rec in records:
        # SD-log format
        if rec.get("event") == "csi":
            detail = rec.get("detail", {})
            nid    = detail.get("node_id", "unknown")
            phases = detail.get("phases", [])
        # UDP-capture format
        elif "phases" in rec:
            nid    = rec.get("node_id", "unknown")
            phases = rec.get("phases", [])
        else:
            continue

        if node_id and nid != node_id:
            continue

        ts_s = rec.get("ts", rec.get("timestamp", 0))
        # Heuristic: timestamps > TIMESTAMP_US_THRESHOLD are assumed to be
        # in microseconds (esp_timer_get_time() output) and are converted to
        # seconds. Timestamps ≤ this value are treated as already in seconds
        # (Unix epoch seconds are ~1.7e9 as of 2024, so we use 1e12 to avoid
        # false matches with future epoch values).
        TIMESTAMP_US_THRESHOLD = 1e12  # values above this are µs, not seconds
        if ts_s > TIMESTAMP_US_THRESHOLD:
            ts_s /= 1e6  # convert µs → s

        for p in (phases if isinstance(phases, list) else [phases]):
            timelines[nid].append((ts_s, float(p)))

    return {nid: np.array(v) for nid, v in timelines.items() if v}


# ------------------------------------------------------------------ #
#  Phase variance analysis                                             #
# ------------------------------------------------------------------ #

def compute_variance_timeline(phase_series: np.ndarray,
                                window: int = 256,
                                step: int = 128) -> np.ndarray:
    """
    Sliding-window phase variance.

    Args:
        phase_series: shape (N, 2) array of [timestamp_s, phase_value]
    Returns:
        shape (M, 2) array of [timestamp_s, variance]
    """
    phases = np.unwrap(phase_series[:, 1])
    times  = phase_series[:, 0]
    result = []
    for i in range(0, len(phases) - window, step):
        var  = float(np.var(phases[i:i + window]))
        ts   = float(np.mean(times[i:i + window]))
        result.append((ts, var))
    return np.array(result) if result else np.empty((0, 2))


# ------------------------------------------------------------------ #
#  Detection events                                                    #
# ------------------------------------------------------------------ #

def extract_detection_events(records: List[Dict]) -> List[Dict]:
    """Return all anomaly_detected marker records."""
    events = []
    for rec in records:
        if rec.get("event") in ("anomaly_detected", "calibrate_start",
                                 "mode_change", "boot", "shutdown"):
            events.append({
                "ts":    rec.get("ts", 0),
                "event": rec.get("event"),
                "detail": rec.get("detail", {}),
            })
    return events


# ------------------------------------------------------------------ #
#  Spectrogram                                                         #
# ------------------------------------------------------------------ #

def compute_spectrogram(phase_series: np.ndarray,
                         fs: float = 1000.0):
    """Compute spectrogram from a phase timeline. Requires scipy."""
    if not SCIPY:
        print("scipy not available — skipping spectrogram", file=sys.stderr)
        return None, None, None

    phases = np.unwrap(phase_series[:, 1])
    phases -= np.mean(phases)
    f, t, Sxx = scipy_signal.spectrogram(phases, fs=fs, nperseg=256, noverlap=128)
    return f, t, Sxx


# ------------------------------------------------------------------ #
#  Plotting                                                            #
# ------------------------------------------------------------------ #

def plot_analysis(timelines: Dict[str, np.ndarray],
                  events: List[Dict],
                  output_prefix: str = "analysis"):
    """Plot variance timelines + event markers for each node."""
    if not MATPLOTLIB:
        print("matplotlib not available — skipping plots", file=sys.stderr)
        return

    for nid, data in timelines.items():
        var_tl = compute_variance_timeline(data)
        if len(var_tl) == 0:
            continue

        fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(14, 8), sharex=True)
        fig.suptitle(f"CSI Log Analysis — {nid}")

        # Phase trace
        ax1.plot(data[:, 0], data[:, 1], lw=0.5, color='steelblue', label='Phase')
        ax1.set_ylabel("Phase (rad)")
        ax1.legend(loc='upper right', fontsize=8)

        # Variance timeline
        ax2.plot(var_tl[:, 0], var_tl[:, 1], lw=1, color='darkorange', label='Variance')
        ax2.set_ylabel("Phase Variance")
        ax2.set_xlabel("Time (s)")

        # Event markers
        for ev in events:
            ts = ev["ts"]
            ax1.axvline(ts, color='red', alpha=0.4, lw=1)
            ax2.axvline(ts, color='red', alpha=0.4, lw=1,
                        label=ev["event"] if ev == events[0] else "")
        ax2.legend(loc='upper right', fontsize=8)

        plt.tight_layout()
        out_path = f"{output_prefix}_{nid}.png"
        plt.savefig(out_path, dpi=120)
        print(f"  Saved: {out_path}")
        plt.close(fig)


# ------------------------------------------------------------------ #
#  Summary                                                             #
# ------------------------------------------------------------------ #

def build_summary(timelines: Dict[str, np.ndarray],
                  events: List[Dict]) -> Dict:
    """Build a JSON-serialisable summary dict."""
    summary = {
        "generated_at": datetime.utcnow().isoformat(),
        "nodes": {},
        "detection_events": events,
    }
    for nid, data in timelines.items():
        var_tl = compute_variance_timeline(data)
        node = {
            "sample_count": int(len(data)),
            "duration_s":   float(data[-1, 0] - data[0, 0]) if len(data) > 1 else 0,
        }
        if len(var_tl) > 0:
            variances = var_tl[:, 1]
            node["variance"] = {
                "mean":  float(np.mean(variances)),
                "std":   float(np.std(variances)),
                "max":   float(np.max(variances)),
                "p95":   float(np.percentile(variances, 95)),
                "p99":   float(np.percentile(variances, 99)),
            }
        summary["nodes"][nid] = node
    return summary


# ------------------------------------------------------------------ #
#  CLI                                                                 #
# ------------------------------------------------------------------ #

def main():
    parser = argparse.ArgumentParser(description="Buddhas-Watch CSI Log Analyzer")
    parser.add_argument("log", help="Path to JSONL log file")
    parser.add_argument("--plot",   action="store_true", help="Generate PNG plots")
    parser.add_argument("--node",   type=str, default=None, help="Filter to a single node_id")
    parser.add_argument("--output", type=str, default=None,  help="Write summary JSON to file")
    parser.add_argument("--fs",     type=float, default=1000.0, help="CSI sampling rate (Hz)")
    args = parser.parse_args()

    log_path = Path(args.log)
    if not log_path.exists():
        print(f"Error: {log_path} not found", file=sys.stderr)
        sys.exit(1)

    print(f"Loading {log_path} ...")
    records = load_jsonl(str(log_path))
    print(f"  {len(records)} records loaded")

    timelines = extract_phase_timeline(records, node_id=args.node)
    events    = extract_detection_events(records)

    print(f"  Nodes: {list(timelines.keys())}")
    print(f"  Detection events: {len(events)}")
    for ev in events[:5]:
        print(f"    ts={ev['ts']:.0f}  {ev['event']}")

    if args.plot:
        prefix = str(log_path.stem)
        print("Generating plots...")
        plot_analysis(timelines, events, output_prefix=prefix)

    summary = build_summary(timelines, events)

    if args.output:
        with open(args.output, "w") as f:
            json.dump(summary, f, indent=2)
        print(f"Summary written to {args.output}")
    else:
        print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
