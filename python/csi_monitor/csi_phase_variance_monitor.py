#!/usr/bin/env python3
"""
csi_phase_variance_monitor.py
Real-time CSI Phase Variance Monitoring for Biological Resonance Detection
Compatible with single-node (ESP32 watch) setups.

Part of Buddhas-Watch — Production-grade distributed CSI collection system.
"""

import socket
import struct
import numpy as np
from collections import deque
import time
import json
import logging
from typing import Optional, Callable

# ====================== CONFIGURATION ======================
UDP_IP = "0.0.0.0"
UDP_PORT = 5500                    # Match your ESP32 sender
WINDOW_SIZE = 256                  # ~1-2 seconds depending on CSI rate
STEP_SIZE = 128                    # 50% overlap
BASELINE_DURATION = 120            # Seconds to learn baseline
DETECTION_THRESHOLD = 3.0          # Standard deviations above baseline
PERSISTENCE_SECONDS = 45           # How long variance must stay elevated

logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)


class CSIPhaseVarianceMonitor:
    def __init__(self,
                 window_size: int = WINDOW_SIZE,
                 baseline_duration: int = BASELINE_DURATION,
                 threshold_sigma: float = DETECTION_THRESHOLD):

        self.window_size = window_size
        self.phase_buffer = deque(maxlen=window_size)
        self.baseline_mean = None
        self.baseline_std = None
        self.is_calibrated = False
        self.calibration_start = time.time()
        self.baseline_duration = baseline_duration
        self.threshold_sigma = threshold_sigma

        self.last_detection_time = 0
        self.detection_active = False
        self.on_detection: Optional[Callable] = None

    def add_phase_sample(self, phase_value: float):
        """Add a new unwrapped phase sample"""
        self.phase_buffer.append(phase_value)

    def compute_phase_variance(self) -> Optional[float]:
        """Compute phase variance over current window"""
        if len(self.phase_buffer) < self.window_size:
            return None
        phase_array = np.array(self.phase_buffer)
        phase_unwrapped = np.unwrap(phase_array)
        variance = np.var(phase_unwrapped)
        return variance

    def update_baseline(self):
        """Learn baseline statistics during initial calibration period"""
        if self.is_calibrated:
            return
        elapsed = time.time() - self.calibration_start
        if elapsed >= self.baseline_duration and len(self.phase_buffer) == self.window_size:
            phase_array = np.array(self.phase_buffer)
            phase_unwrapped = np.unwrap(phase_array)
            self.baseline_mean = np.mean(phase_unwrapped)
            self.baseline_std = np.std(phase_unwrapped)
            self.is_calibrated = True
            logger.info(f"Baseline calibrated: mean={self.baseline_mean:.4f}, std={self.baseline_std:.4f}")

    def check_for_detection(self, current_variance: float, current_freq: Optional[float] = None) -> bool:
        """Check if current variance exceeds threshold with persistence gating"""
        if not self.is_calibrated or self.baseline_std is None or self.baseline_std == 0:
            return False
        z_score = (current_variance - self.baseline_mean) / self.baseline_std
        if z_score > self.threshold_sigma:
            current_time = time.time()
            if not self.detection_active:
                self.detection_active = True
                self.last_detection_time = current_time
                self._trigger_detection(current_variance, z_score, current_freq)
                return True
            else:
                if current_time - self.last_detection_time > PERSISTENCE_SECONDS:
                    self._trigger_detection(current_variance, z_score, current_freq)
                    self.last_detection_time = current_time
                    return True
        else:
            self.detection_active = False
        return False

    def _trigger_detection(self, variance: float, z_score: float, freq: Optional[float]):
        """Handle detection event — log and fire callback"""
        msg = f"PHASE VARIANCE DETECTION | Variance: {variance:.4f} | Z-Score: {z_score:.2f}"
        if freq:
            msg += f" | Frequency: {freq}"
        logger.warning(msg)
        if self.on_detection:
            self.on_detection(variance, z_score, freq)

    def process_sample(self, phase_value: float, current_freq: Optional[float] = None) -> Optional[float]:
        """Main processing function. Call for every new CSI phase sample."""
        self.add_phase_sample(phase_value)
        self.update_baseline()
        variance = self.compute_phase_variance()
        if variance is not None and self.is_calibrated:
            self.check_for_detection(variance, current_freq)
            return variance
        return None


# ====================== EXAMPLE USAGE ======================

def example_udp_listener():
    monitor = CSIPhaseVarianceMonitor()

    def detection_callback(variance, z_score, freq):
        print(f"*** ALERT: High phase variance detected at {freq} Hz ***")

    monitor.on_detection = detection_callback

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((UDP_IP, UDP_PORT))
    sock.settimeout(1.0)

    logger.info("Starting CSI Phase Variance Monitor...")

    try:
        while True:
            try:
                data, addr = sock.recvfrom(4096)
                try:
                    packet = json.loads(data.decode())
                    phase = float(packet.get('phase', 0))
                    freq = packet.get('frequency', None)
                    variance = monitor.process_sample(phase, freq)
                except (json.JSONDecodeError, KeyError, ValueError):
                    continue
            except socket.timeout:
                continue
    except KeyboardInterrupt:
        logger.info("Monitor stopped by user.")


if __name__ == "__main__":
    example_udp_listener()
