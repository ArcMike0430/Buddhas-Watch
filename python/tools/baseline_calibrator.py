#!/usr/bin/env python3
"""
baseline_calibrator.py
Records baseline phase variance from ESP32 watches and saves to JSON.
Run once per deployment environment — establishes noise floor for detection.

Output: baseline.json — loaded by csi_phase_variance_monitor.py on startup.

Usage:
    # 2-minute calibration, all nodes
    python baseline_calibrator.py --duration 120
    
    # 5-minute calibration, single node
    python baseline_calibrator.py --duration 300 --node watch_left
    
    # Load and visualize existing baseline
    python baseline_calibrator.py --visualize baseline.json
"""

import argparse
import json
import socket
import time
import numpy as np
from collections import defaultdict, deque
from datetime import datetime
from typing import Dict, List, Optional


# ===================== UDP LISTENER =====================
class CSIListener:
    """Listens for UDP CSI packets from ESP32 watches."""
    
    def __init__(self, port: int = 5500, buffer_seconds: float = 10.0):
        self.port = port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(('0.0.0.0', port))
        self.sock.settimeout(0.5)
        
        # Per-node buffers
        self.buffers: Dict[str, deque] = defaultdict(
            lambda: deque(maxlen=20000)
        )
        self.packet_count: Dict[str, int] = defaultdict(int)
        self.start_time = time.time()
    
    def collect(self, duration_seconds: float) -> Dict[str, np.ndarray]:
        """Collect CSI data for duration_seconds. Returns {node_id: phase_array}."""
        print(f"Collecting CSI data for {duration_seconds}s on port {self.port}...")
        print("Press Ctrl+C to stop early.\n")
        
        end_time = time.time() + duration_seconds
        
        while time.time() < end_time:
            try:
                data, addr = self.sock.recvfrom(4096)
                packet = json.loads(data.decode('utf-8'))
                node_id = packet.get('node_id', 'unknown')
                
                phases = packet.get('phases', []) or packet.get('phase', [])
                # Handle both single phase and array
                if isinstance(phases, list):
                    for p in phases:
                        if isinstance(p, (int, float)):
                            self.buffers[node_id].append(p)
                elif isinstance(phases, (int, float)):
                    self.buffers[node_id].append(phases)
                
                self.packet_count[node_id] += 1
                
            except socket.timeout:
                continue
            except json.JSONDecodeError:
                continue
        
        # Convert to numpy arrays
        result = {}
        for node_id, buf in self.buffers.items():
            if len(buf) > 0:
                result[node_id] = np.array(list(buf))
                print(f"  {node_id}: {len(buf)} samples, {self.packet_count[node_id]} packets")
        
        return result


# ===================== ANALYSIS =====================
def compute_baseline_stats(
    data: Dict[str, np.ndarray],
    fs: float = 1000.0
) -> Dict:
    """Compute baseline statistics from collected CSI data."""
    
    baseline = {
        "timestamp": datetime.utcnow().isoformat(),
        "sampling_rate_hz": fs,
        "nodes": {}
    }
    
    for node_id, phases in data.items():
        if len(phases) < 100:
            continue
        
        # Unwrap
        unwrapped = np.unwrap(phases)
        
        # Phase variance statistics
        window = 256
        step = 128
        variances = []
        
        for i in range(0, len(unwrapped) - window, step):
            var = np.var(unwrapped[i:i + window])
            variances.append(var)
        
        variances = np.array(variances)
        
        # FFT-based noise floor
        fft = np.fft.rfft(unwrapped - np.mean(unwrapped))
        power = np.abs(fft) ** 2
        noise_floor = np.median(power)
        
        # Spectral entropy
        power_norm = power / (power.sum() + 1e-12)
        entropy = -np.sum(power_norm * np.log2(power_norm + 1e-12))
        max_entropy = np.log2(len(power_norm))
        coherence = 1.0 - (entropy / max_entropy) if max_entropy > 0 else 0.0
        
        node_stats = {
            "sample_count": int(len(phases)),
            "phase_variance": {
                "mean": float(np.mean(variances)),
                "std": float(np.std(variances)),
                "min": float(np.min(variances)),
                "max": float(np.max(variances)),
                "p50": float(np.percentile(variances, 50)),
                "p95": float(np.percentile(variances, 95)),
                "p99": float(np.percentile(variances, 99)),
            },
            "fft_noise_floor": float(noise_floor),
            "spectral_coherence": float(coherence),
            "recommended_threshold_sigma": 3.0,
            "recommended_persistence_frames": 5,
        }
        
        baseline["nodes"][node_id] = node_stats
        
        print(f"\n{node_id}:")
        print(f"  Samples:       {node_stats['sample_count']}")
        print(f"  Variance mean: {node_stats['phase_variance']['mean']:.6f}")
        print(f"  Variance std:  {node_stats['phase_variance']['std']:.6f}")
        print(f"  Variance p95:  {node_stats['phase_variance']['p95']:.6f}")
        print(f"  Noise floor:   {node_stats['fft_noise_floor']:.6e}")
        print(f"  Coherence:     {node_stats['spectral_coherence']:.3f}")
        print(f"  Threshold (3σ): {node_stats['phase_variance']['mean'] + 3 * node_stats['phase_variance']['std']:.6f}")
    
    return baseline


# ===================== VISUALIZATION =====================
def visualize_baseline(path: str):
    """Print a human-readable summary of a saved baseline."""
    with open(path, 'r') as f:
        data = json.load(f)
    
    print(f"Baseline recorded: {data['timestamp']}")
    print(f"Sampling rate: {data['sampling_rate_hz']} Hz")
    print()
    
    for node_id, stats in data.get('nodes', {}).items():
        print(f"=== {node_id} ===")
        print(f"  Samples:          {stats['sample_count']}")
        print(f"  Variance mean:    {stats['phase_variance']['mean']:.6f}")
        print(f"  Variance std:     {stats['phase_variance']['std']:.6f}")
        print(f"  Variance P95:     {stats['phase_variance']['p95']:.6f}")
        print(f"  Variance P99:     {stats['phase_variance']['p99']:.6f}")
        print(f"  Noise floor:      {stats['fft_noise_floor']:.2e}")
        print(f"  Coherence:        {stats['spectral_coherence']:.3f}")
        print(f"  Threshold 3σ:     {stats['phase_variance']['mean'] + 3 * stats['phase_variance']['std']:.6f}")
        print()


# ===================== MAIN =====================
def main():
    parser = argparse.ArgumentParser(
        description="Buddhas-Watch Baseline Calibrator"
    )
    parser.add_argument(
        '--duration', type=int, default=120,
        help='Calibration duration in seconds (default: 120)'
    )
    parser.add_argument(
        '--output', type=str, default='baseline.json',
        help='Output file (default: baseline.json)'
    )
    parser.add_argument(
        '--visualize', type=str, default=None,
        help='Visualize existing baseline file instead of collecting'
    )
    parser.add_argument(
        '--port', type=int, default=5500,
        help='UDP listen port (default: 5500)'
    )
    
    args = parser.parse_args()
    
    if args.visualize:
        visualize_baseline(args.visualize)
        return
    
    print(f"\nBuddhas-Watch Baseline Calibrator")
    print(f"Collecting for {args.duration}s...\n")
    
    listener = CSIListener(port=args.port)
    data = listener.collect(args.duration)
    
    if not data:
        print("No data received. Check that ESP32 watches are streaming.")
        return
    
    baseline = compute_baseline_stats(data)
    
    with open(args.output, 'w') as f:
        json.dump(baseline, f, indent=2)
    
    print(f"\nBaseline saved to {args.output}")
    print("Use this file with csi_phase_variance_monitor.py:")
    print(f"  --baseline {args.output}")


if __name__ == '__main__':
    main()
