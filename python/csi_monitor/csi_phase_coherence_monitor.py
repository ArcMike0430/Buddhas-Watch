#!/usr/bin/env python3
"""
csi_phase_coherence_monitor.py
Real-time CSI Phase Variance + Cross-Subcarrier Phase Coherence Monitoring
Optimized for single-node or multi-node biological resonance detection.

Part of Buddhas-Watch — Production-grade distributed CSI collection system.
"""

import socket
import json
import numpy as np
from collections import deque
import time
import logging
from typing import Optional, Callable, List

logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)


class CSIPhaseCoherenceMonitor:
    def __init__(self,
                 window_size: int = 256,
                 baseline_duration: int = 120,
                 variance_threshold_sigma: float = 3.0,
                 coherence_threshold: float = 0.75):

        self.window_size = window_size
        self.phase_buffer = deque(maxlen=window_size)
        self.baseline_mean = None
        self.baseline_std = None
        self.is_calibrated = False
        self.calibration_start = time.time()
        self.baseline_duration = baseline_duration
        self.variance_threshold_sigma = variance_threshold_sigma
        self.coherence_threshold = coherence_threshold

        self.last_detection_time = 0
        self.detection_active = False
        self.on_detection: Optional[Callable] = None

    # ------------------ Phase Variance ------------------
    def add_phase_sample(self, phase_value: float):
        self.phase_buffer.append(phase_value)

    def compute_phase_variance(self) -> Optional[float]:
        if len(self.phase_buffer) < self.window_size:
            return None
        phase_array = np.array(self.phase_buffer)
        phase_unwrapped = np.unwrap(phase_array)
        return np.var(phase_unwrapped)

    # ------------------ Cross-Subcarrier Coherence ------------------
    def compute_phase_coherence(self, subcarrier_phases: List[float]) -> Optional[float]:
        """Compute magnitude-squared coherence across multiple subcarriers."""
        if len(subcarrier_phases) < 2:
            return None
        phases = np.array(subcarrier_phases)
        phases_unwrapped = np.unwrap(phases)
        mean_phase = np.mean(phases_unwrapped)
        coherence = np.abs(np.mean(np.exp(1j * (phases_unwrapped - mean_phase))))
        return coherence

    # ------------------ Baseline & Detection ------------------
    def update_baseline(self):
        if self.is_calibrated:
            return
        elapsed = time.time() - self.calibration_start
        if elapsed >= self.baseline_duration and len(self.phase_buffer) == self.window_size:
            phase_array = np.array(self.phase_buffer)
            phase_unwrapped = np.unwrap(phase_array)
            self.baseline_mean = np.mean(phase_unwrapped)
            self.baseline_std = np.std(phase_unwrapped)
            self.is_calibrated = True
            logger.info(f"Baseline calibrated | mean={self.baseline_mean:.4f}, std={self.baseline_std:.4f}")

    def check_detection(self, variance: float, coherence: Optional[float] = None,
                        freq: Optional[float] = None) -> bool:
        if not self.is_calibrated or self.baseline_std == 0:
            return False
        z_score = (variance - self.baseline_mean) / self.baseline_std
        triggered = False
        if z_score > self.variance_threshold_sigma:
            triggered = True
        if coherence is not None and coherence > self.coherence_threshold:
            triggered = True
        if triggered:
            current_time = time.time()
            if not self.detection_active or (current_time - self.last_detection_time > 45):
                self.detection_active = True
                self.last_detection_time = current_time
                self._trigger_detection(variance, z_score, coherence, freq)
                return True
        else:
            self.detection_active = False
        return False

    def _trigger_detection(self, variance, z_score, coherence, freq):
        msg = f"DETECTION | Variance={variance:.4f} (Z={z_score:.2f})"
        if coherence is not None:
            msg += f" | Coherence={coherence:.3f}"
        if freq:
            msg += f" | Freq={freq}"
        logger.warning(msg)
        if self.on_detection:
            self.on_detection(variance, z_score, coherence, freq)

    # ------------------ Main Processing ------------------
    def process_sample(self,
                       phase_value: float,
                       subcarrier_phases: Optional[List[float]] = None,
                       current_freq: Optional[float] = None) -> dict:
        """Process one CSI sample. Returns dict with variance, coherence, detection status."""
        self.add_phase_sample(phase_value)
        self.update_baseline()
        result = {"variance": None, "coherence": None, "detected": False, "z_score": None}
        variance = self.compute_phase_variance()
        if variance is None:
            return result
        coherence = None
        if subcarrier_phases and len(subcarrier_phases) >= 2:
            coherence = self.compute_phase_coherence(subcarrier_phases)
        detected = self.check_detection(variance, coherence, current_freq)
        result.update({
            "variance": variance,
            "coherence": coherence,
            "detected": detected,
            "z_score": (variance - self.baseline_mean) / self.baseline_std if self.is_calibrated else None
        })
        return result


# ====================== EXAMPLE INTEGRATION ======================

def example_with_spectrogram():
    monitor = CSIPhaseCoherenceMonitor()

    def alert_callback(variance, z_score, coherence, freq):
        print(f">>> HIGH PHASE ACTIVITY DETECTED at {freq} <<<")

    monitor.on_detection = alert_callback

    for i in range(500):
        phase = np.random.normal(0, 0.1) + (0.8 if 200 < i < 250 else 0)
        sub_phases = [phase + np.random.normal(0, 0.05) for _ in range(8)]
        result = monitor.process_sample(phase, sub_phases, current_freq=1.2e6)
        if result["detected"]:
            print(f"Detection at sample {i}: {result}")
        time.sleep(0.01)


if __name__ == "__main__":
    example_with_spectrogram()
