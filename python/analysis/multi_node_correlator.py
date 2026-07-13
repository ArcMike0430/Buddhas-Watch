#!/usr/bin/env python3
"""
multi_node_correlator.py
Cross-node phase correlation and synthetic aperture analysis.

Compares phase timelines from two or more watch nodes to:
  - Compute cross-correlation of phase variance signals
  - Detect events that appear coherently across nodes (high priority)
  - Compute time-of-arrival (TOA) differences for rough source localisation

Usage:
    python multi_node_correlator.py --left udp_capture_left.jsonl \
                                    --right udp_capture_right.jsonl
"""

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import numpy as np

try:
    from scipy.signal import correlate, correlation_lags
    SCIPY = True
except ImportError:
    SCIPY = False


# ------------------------------------------------------------------ #
#  Load phase samples from a JSONL capture file                        #
# ------------------------------------------------------------------ #

def load_phase_samples(path: str, fs: float = 1000.0) -> Tuple[np.ndarray, np.ndarray]:
    """
    Load a JSONL file of CSI packets and return (timestamps, phases).
    Phases are the mean across all 52 subcarriers per packet.
    """
    times, phases = [], []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                pkt = json.loads(line)
            except json.JSONDecodeError:
                continue
            raw_phases = pkt.get("phases", [])
            if not raw_phases:
                continue
            ts_us = pkt.get("timestamp", len(times) * (1e6 / fs))
            ts_s  = float(ts_us) / 1e6 if ts_us > 1e7 else float(ts_us)
            times.append(ts_s)
            phases.append(float(np.mean(raw_phases)))
    return np.array(times), np.unwrap(np.array(phases))


# ------------------------------------------------------------------ #
#  Resample to a common uniform grid                                   #
# ------------------------------------------------------------------ #

def resample_to_grid(times: np.ndarray,
                      values: np.ndarray,
                      fs: float = 1000.0) -> np.ndarray:
    """Interpolate irregular time series onto a uniform grid at fs Hz."""
    if len(times) < 2:
        return values
    t_start = times[0]
    t_end   = times[-1]
    t_grid  = np.arange(t_start, t_end, 1.0 / fs)
    return np.interp(t_grid, times, values)


# ------------------------------------------------------------------ #
#  Cross-correlation                                                   #
# ------------------------------------------------------------------ #

def cross_correlate_nodes(sig_a: np.ndarray,
                            sig_b: np.ndarray,
                            fs: float = 1000.0) -> Dict:
    """
    Compute normalised cross-correlation between two phase signals.

    Returns:
        peak_lag_ms: lag at maximum correlation (positive = B lags A)
        peak_corr:   normalised peak correlation value [-1, 1]
        lags_ms:     array of lag values in milliseconds
        corr:        full correlation array
    """
    n = min(len(sig_a), len(sig_b))
    a = sig_a[:n] - np.mean(sig_a[:n])
    b = sig_b[:n] - np.mean(sig_b[:n])

    if SCIPY:
        corr = correlate(a, b, mode='full')
        lags = correlation_lags(len(a), len(b), mode='full') / fs * 1000.0
    else:
        corr  = np.correlate(a, b, mode='full')
        n_lag = len(corr)
        lags  = (np.arange(n_lag) - n_lag // 2) / fs * 1000.0

    # Normalise
    norm = np.sqrt(np.sum(a ** 2) * np.sum(b ** 2)) + 1e-12
    corr = corr / norm

    peak_idx    = int(np.argmax(np.abs(corr)))
    peak_lag_ms = float(lags[peak_idx])
    peak_corr   = float(corr[peak_idx])

    return {
        "peak_lag_ms": peak_lag_ms,
        "peak_corr":   peak_corr,
        "lags_ms":     lags,
        "corr":        corr,
    }


# ------------------------------------------------------------------ #
#  Sliding-window variance                                             #
# ------------------------------------------------------------------ #

def sliding_variance(signal: np.ndarray,
                      window: int = 256,
                      step: int = 128) -> np.ndarray:
    return np.array([
        np.var(signal[i:i + window])
        for i in range(0, len(signal) - window, step)
    ])


# ------------------------------------------------------------------ #
#  Coincidence detection                                               #
# ------------------------------------------------------------------ #

def find_coincident_anomalies(var_a: np.ndarray,
                               var_b: np.ndarray,
                               threshold_sigma: float = 3.0,
                               max_lag_bins: int = 5) -> List[int]:
    """
    Find time-bin indices where BOTH nodes show elevated variance
    within max_lag_bins of each other — indicating a real event
    rather than node-local noise.

    Returns list of bin indices where coincidences were found.
    """
    n = min(len(var_a), len(var_b))
    mu_a, std_a = np.mean(var_a), np.std(var_a) + 1e-12
    mu_b, std_b = np.mean(var_b), np.std(var_b) + 1e-12

    z_a = (var_a[:n] - mu_a) / std_a
    z_b = (var_b[:n] - mu_b) / std_b

    hits = []
    for i in range(n):
        if z_a[i] > threshold_sigma:
            # Look for matching hit in B within ±max_lag_bins
            lo = max(0, i - max_lag_bins)
            hi = min(n - 1, i + max_lag_bins)
            if np.any(z_b[lo:hi + 1] > threshold_sigma):
                hits.append(i)
    return hits


# ------------------------------------------------------------------ #
#  CLI                                                                 #
# ------------------------------------------------------------------ #

def main():
    parser = argparse.ArgumentParser(
        description="Buddhas-Watch Multi-Node Phase Correlator"
    )
    parser.add_argument("--left",  required=True, help="JSONL capture for left watch")
    parser.add_argument("--right", required=True, help="JSONL capture for right watch")
    parser.add_argument("--fs",    type=float, default=1000.0, help="Sampling rate Hz")
    parser.add_argument("--sigma", type=float, default=3.0,   help="Detection threshold σ")
    args = parser.parse_args()

    print("Loading left watch data ...")
    t_l, p_l = load_phase_samples(args.left,  args.fs)
    print(f"  {len(p_l)} samples")

    print("Loading right watch data ...")
    t_r, p_r = load_phase_samples(args.right, args.fs)
    print(f"  {len(p_r)} samples")

    # Resample to uniform grids
    p_l_r = resample_to_grid(t_l, p_l, args.fs)
    p_r_r = resample_to_grid(t_r, p_r, args.fs)

    # Cross-correlation
    print("\nCross-correlation:")
    cc = cross_correlate_nodes(p_l_r, p_r_r, args.fs)
    print(f"  Peak lag:  {cc['peak_lag_ms']:.2f} ms")
    print(f"  Peak corr: {cc['peak_corr']:.4f}")

    # Variance timelines
    var_l = sliding_variance(p_l_r)
    var_r = sliding_variance(p_r_r)

    # Coincidence detection
    hits = find_coincident_anomalies(var_l, var_r, threshold_sigma=args.sigma)
    print(f"\nCoincident anomaly bins ({args.sigma}σ threshold): {len(hits)}")
    for h in hits[:10]:
        print(f"  bin {h}: var_left={var_l[h]:.4e}, var_right={var_r[h]:.4e}")

    summary = {
        "cross_correlation": {
            "peak_lag_ms": cc["peak_lag_ms"],
            "peak_corr":   cc["peak_corr"],
        },
        "coincident_anomalies": len(hits),
        "anomaly_bins":         hits[:50],
    }
    print("\nSummary:")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
