#!/usr/bin/env python3
"""
quantum_enhanced_detection.py
Quantum-like phase coherence analysis across all 52 subcarriers.
Models subcarrier phase relationships as an entangled-analog state
to detect structured interference invisible to classical FFT.

Runs on Jetson GPU — Qiskit Aer backend for statevector simulation.

Usage:
    from quantum_enhanced_detection import QuantumCoherenceDetector
    
    detector = QuantumCoherenceDetector()
    result = detector.analyze_frame(subcarrier_phases_52xN)
    if result["anomaly"]:
        print(f"Phase coherence collapse at frame {frame}")
"""

import numpy as np
from typing import Optional, List, Dict

try:
    from qiskit import QuantumCircuit, transpile
    from qiskit_aer import AerSimulator
    from qiskit.quantum_info import Statevector, state_fidelity
    QISKIT_AVAILABLE = True
except ImportError:
    QISKIT_AVAILABLE = False


class QuantumCoherenceDetector:
    """
    Detects phase coherence anomalies across subcarriers using
    quantum-like computation on classical hardware.
    
    The key insight: 52 subcarriers × phase values naturally form
    a quantum-like state vector. When interference is present,
    the phase coherence structure breaks in ways that classical
    per-subcarrier FFT cannot detect.

    Implementation:
        1. Map subcarrier phase differences to Ry rotation angles on
           n_qubits qubits — each angle encodes a fraction of the
           phase-difference distribution.
        2. Apply Hadamard + Ry gates to build a parameterised state.
        3. Simulate with AerSimulator (statevector method, GPU if available).
        4. Extract statevector and compute von-Neumann-style entropy as
           the coherence metric.
    """
    
    def __init__(self, n_qubits: int = 6, coherence_threshold: float = 0.65):
        """
        n_qubits: qubits for the quantum circuit (6 → 64-dim state)
        coherence_threshold: below this value, the frame is flagged
        """
        self.n_qubits = min(n_qubits, 12)   # Cap for Jetson performance
        self.coherence_threshold = coherence_threshold
        self.baseline_coherence = None
        self.baseline_frames = 0
        self.baseline_target = 50

        self.simulator = None
        if QISKIT_AVAILABLE:
            try:
                self.simulator = AerSimulator(method='statevector', device='GPU')
                print(f"QuantumCoherenceDetector: Qiskit GPU backend ready ({self.n_qubits} qubits)")
            except Exception:
                self.simulator = AerSimulator(method='statevector')
                print(f"QuantumCoherenceDetector: Qiskit CPU backend ready ({self.n_qubits} qubits)")
        else:
            print("QuantumCoherenceDetector: Qiskit not available, using classical fallback")

    # ------------------------------------------------------------------ #
    #  Phase → quantum circuit                                             #
    # ------------------------------------------------------------------ #

    def _phases_to_angles(self, subcarrier_phases: np.ndarray) -> np.ndarray:
        """
        Convert 52 subcarrier phases into n_qubits Ry rotation angles.

        Steps:
          1. Unwrap phases and compute adjacent differences.
          2. Normalise differences to [0, π] (valid Ry range).
          3. Bin into n_qubits values by computing the mean difference
             within each equally-sized sub-band.
        """
        phases = np.unwrap(np.array(subcarrier_phases, dtype=np.float64))
        diffs  = np.diff(phases)
        # Wrap to [0, π] via abs then rescale
        diffs  = np.abs(np.arctan2(np.sin(diffs), np.cos(diffs)))

        # Bin into n_qubits values
        bin_size = max(1, len(diffs) // self.n_qubits)
        angles   = np.array([
            np.mean(diffs[i * bin_size: (i + 1) * bin_size])
            for i in range(self.n_qubits)
        ])
        return angles

    def _build_circuit(self, angles: np.ndarray) -> "QuantumCircuit":
        """
        Build a parameterised quantum circuit from Ry angles.

        Architecture:
            H  — put each qubit in uniform superposition
            Ry(θ) — rotate by phase-derived angle (encodes distribution)
            CX chain — entangle adjacent qubits (captures correlations)
        """
        qc = QuantumCircuit(self.n_qubits)

        # Layer 1: Hadamard to create superposition baseline
        for i in range(self.n_qubits):
            qc.h(i)

        # Layer 2: Ry rotations encoding the phase-difference distribution
        for i, theta in enumerate(angles):
            qc.ry(float(theta), i)

        # Layer 3: CNOT chain — captures inter-subcarrier correlations
        for i in range(self.n_qubits - 1):
            qc.cx(i, i + 1)

        # Save statevector
        qc.save_statevector()

        return qc

    def _run_circuit(self, qc: "QuantumCircuit") -> np.ndarray:
        """Transpile, run on AerSimulator, return probability amplitudes."""
        compiled = transpile(qc, self.simulator, optimization_level=0)
        job      = self.simulator.run(compiled, shots=1)
        result   = job.result()
        sv       = result.get_statevector(compiled)
        # Return probability distribution (|amplitude|²)
        return np.abs(np.array(sv)) ** 2

    def _entropy_coherence(self, probs: np.ndarray) -> float:
        """
        Convert a probability distribution into a coherence score [0, 1].
        
        High coherence (peaked distribution) → low entropy → score near 1.
        Decoherent (flat distribution) → high entropy → score near 0.
        """
        probs   = probs / (probs.sum() + 1e-12)
        entropy = -np.sum(probs * np.log2(probs + 1e-12))
        max_ent = np.log2(len(probs))
        return float(1.0 - entropy / max_ent) if max_ent > 0 else 0.0

    # ------------------------------------------------------------------ #
    #  Public API                                                          #
    # ------------------------------------------------------------------ #

    def analyze_frame(self, subcarrier_phases: np.ndarray) -> Dict:
        """
        Analyze one frame of subcarrier phase data.
        
        Args:
            subcarrier_phases: Array of shape (52,) or (52, N) with phase values
            
        Returns:
            Dict with keys: coherence, anomaly, delta_from_baseline,
                            calibrating, [frames_remaining | baseline]
        """
        if subcarrier_phases.ndim > 1:
            phases = np.mean(subcarrier_phases, axis=1)
        else:
            phases = subcarrier_phases

        if len(phases) < 2:
            return {"coherence": 0.0, "anomaly": False, "delta_from_baseline": 0.0}

        if QISKIT_AVAILABLE and self.simulator is not None:
            angles = self._phases_to_angles(phases)
            qc     = self._build_circuit(angles)
            probs  = self._run_circuit(qc)
            coherence = self._entropy_coherence(probs)
        else:
            # Classical fallback via spectral entropy
            coherence = ClassicalCoherenceDetector._compute_coherence_static(phases)

        # Baseline calibration
        if self.baseline_frames < self.baseline_target:
            if self.baseline_coherence is None:
                self.baseline_coherence = coherence
            else:
                self.baseline_coherence = 0.9 * self.baseline_coherence + 0.1 * coherence
            self.baseline_frames += 1
            return {
                "coherence": coherence,
                "anomaly": False,
                "delta_from_baseline": 0.0,
                "calibrating": True,
                "frames_remaining": self.baseline_target - self.baseline_frames
            }

        delta   = coherence - self.baseline_coherence
        anomaly = (abs(delta) > (1.0 - self.coherence_threshold)
                   or coherence < self.coherence_threshold)

        return {
            "coherence": coherence,
            "anomaly": anomaly,
            "delta_from_baseline": float(delta),
            "calibrating": False,
            "baseline": float(self.baseline_coherence)
        }


# ====================== CLASSICAL FALLBACK ======================

class ClassicalCoherenceDetector:
    """
    Classical fallback when Qiskit is not available.
    Uses numpy FFT + entropy-based coherence instead of quantum simulation.
    
    Structurally equivalent to QuantumCoherenceDetector but runs
    on CPU with no dependencies beyond numpy.
    """
    
    def __init__(self, coherence_threshold: float = 0.65):
        self.coherence_threshold = coherence_threshold
        self.baseline_coherence  = None
        self.baseline_frames     = 0
        self.baseline_target     = 50

    @staticmethod
    def _compute_coherence_static(phases: np.ndarray) -> float:
        """Compute phase coherence using FFT spectral entropy (static helper)."""
        phases = np.unwrap(np.array(phases, dtype=np.float64))
        diffs  = np.diff(phases)
        diffs  = np.arctan2(np.sin(diffs), np.cos(diffs))
        fft    = np.fft.rfft(diffs)
        power  = np.abs(fft) ** 2
        power  = power / (power.sum() + 1e-12)
        entropy = -np.sum(power * np.log2(power + 1e-12))
        max_ent = np.log2(len(power))
        return float(1.0 - entropy / max_ent) if max_ent > 0 else 0.0

    def _compute_coherence(self, phases: np.ndarray) -> float:
        return self._compute_coherence_static(phases)

    def analyze_frame(self, subcarrier_phases: np.ndarray) -> Dict:
        if subcarrier_phases.ndim > 1:
            phases = np.mean(subcarrier_phases, axis=1)
        else:
            phases = subcarrier_phases

        coherence = self._compute_coherence(phases)

        if self.baseline_frames < self.baseline_target:
            if self.baseline_coherence is None:
                self.baseline_coherence = coherence
            else:
                self.baseline_coherence = 0.9 * self.baseline_coherence + 0.1 * coherence
            self.baseline_frames += 1
            return {
                "coherence": coherence,
                "anomaly": False,
                "delta": 0.0,
                "calibrating": True,
                "remaining": self.baseline_target - self.baseline_frames
            }

        delta   = coherence - self.baseline_coherence
        anomaly = coherence < self.coherence_threshold

        return {
            "coherence": coherence,
            "anomaly": anomaly,
            "delta": float(delta),
            "calibrating": False,
            "baseline": float(self.baseline_coherence)
        }


# ====================== FACTORY ======================

def create_detector(use_quantum: bool = True) -> object:
    """Factory: returns QuantumCoherenceDetector or ClassicalCoherenceDetector."""
    if use_quantum and QISKIT_AVAILABLE:
        return QuantumCoherenceDetector()
    return ClassicalCoherenceDetector()


# ====================== TEST ======================
if __name__ == "__main__":
    detector = create_detector(use_quantum=False)
    print("Testing classical coherence detector...")

    for i in range(100):
        # Anomaly at frame 70-80: random phase noise across all subcarriers
        if 70 <= i <= 80:
            phases = np.random.uniform(-0.5, 0.5, 52)
        else:
            # Normal: smooth phase progression with small noise
            phases = np.linspace(0, 0.5, 52) + np.random.normal(0, 0.02, 52)

        result = detector.analyze_frame(phases)

        if result.get("anomaly"):
            print(f"Frame {i}: ANOMALY — coherence={result['coherence']:.3f}, "
                  f"delta={result.get('delta', 0):.3f}")

    print("Done.")

