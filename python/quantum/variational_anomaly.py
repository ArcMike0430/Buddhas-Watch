#!/usr/bin/env python3
"""
variational_anomaly.py
VQE-inspired variational anomaly threshold learner.

Uses a Variational Quantum Eigensolver (VQE) style approach to learn
an optimal anomaly decision boundary from labelled CSI phase frames.
At inference time the trained parameters define a Hamiltonian whose
ground-state energy separates normal from anomalous frames.

Running on CPU (small circuits) or Jetson GPU (larger circuits).

Usage:
    from variational_anomaly import VariationalAnomalyDetector

    vad = VariationalAnomalyDetector(n_qubits=4)
    vad.fit(normal_frames, anomaly_frames)          # train
    label = vad.predict(new_frame)                  # "normal" | "anomaly"
"""

import numpy as np
from typing import List, Optional, Tuple

try:
    from qiskit import QuantumCircuit, transpile
    from qiskit_aer import AerSimulator
    from qiskit.quantum_info import SparsePauliOp
    from scipy.optimize import minimize
    QISKIT_AVAILABLE = True
except ImportError:
    QISKIT_AVAILABLE = False


# ------------------------------------------------------------------ #
#  Variational quantum circuit (ansatz)                                #
# ------------------------------------------------------------------ #

def _build_ansatz(n_qubits: int, params: np.ndarray) -> "QuantumCircuit":
    """
    Hardware-efficient ansatz: alternating Ry layers + CNOT chains.

    params layout: [ry_layer0_q0, ry_layer0_q1, ..., ry_layer1_q0, ...]
    Total parameters = n_qubits * n_layers (n_layers = 2 fixed).
    """
    n_layers = 2
    assert len(params) == n_qubits * n_layers, \
        f"Expected {n_qubits * n_layers} params, got {len(params)}"

    qc = QuantumCircuit(n_qubits)
    idx = 0
    for layer in range(n_layers):
        # Ry rotation layer
        for q in range(n_qubits):
            qc.ry(float(params[idx]), q)
            idx += 1
        # Entangling CNOT chain
        for q in range(n_qubits - 1):
            qc.cx(q, q + 1)

    return qc


# ------------------------------------------------------------------ #
#  Hamiltonian: Z⊗Z on all adjacent pairs (Ising-like)                #
# ------------------------------------------------------------------ #

def _build_hamiltonian(n_qubits: int) -> "SparsePauliOp":
    """
    Ising Hamiltonian: H = Σ_{i} Z_i Z_{i+1}
    Ground state energy ≈ −(n_qubits − 1) for a fully correlated state.
    Excited states have higher energy → anomalous frames raise the energy.
    """
    terms = []
    for i in range(n_qubits - 1):
        pauli_str = "I" * (n_qubits - 2 - i) + "ZZ" + "I" * i
        terms.append((pauli_str, 1.0))
    return SparsePauliOp.from_list(terms)


# ------------------------------------------------------------------ #
#  Expectation value via Aer statevector                               #
# ------------------------------------------------------------------ #

def _expectation(params: np.ndarray,
                 features: np.ndarray,
                 n_qubits: int,
                 hamiltonian: "SparsePauliOp",
                 simulator: "AerSimulator") -> float:
    """
    Compute ⟨ψ(params + features)|H|ψ(params + features)⟩.

    Features are added to the first layer of params so the circuit
    is conditioned on the input data.
    """
    combined = params.copy()
    n_feat   = min(len(features), n_qubits)
    combined[:n_feat] += features[:n_feat]

    qc = _build_ansatz(n_qubits, combined)
    qc.save_statevector()

    compiled = transpile(qc, simulator, optimization_level=0)
    result   = simulator.run(compiled, shots=1).result()
    sv       = np.array(result.get_statevector(compiled))

    # Compute expectation value manually from statevector
    # E = ⟨sv| H |sv⟩ using the sparse Pauli representation
    exp_val = 0.0
    for pauli, coeff in zip(hamiltonian.paulis, hamiltonian.coeffs):
        # Apply Pauli to statevector and compute overlap
        pauli_mat  = pauli.to_matrix()
        exp_val   += float(np.real(coeff * (sv.conj() @ pauli_mat @ sv)))

    return exp_val


# ------------------------------------------------------------------ #
#  Main detector                                                       #
# ------------------------------------------------------------------ #

class VariationalAnomalyDetector:
    """
    Learns an anomaly decision threshold using a VQE-style optimisation.

    After fitting, `predict()` runs the variational circuit for a new frame
    and compares its energy to the learned threshold.
    """

    def __init__(self, n_qubits: int = 4, n_iters: int = 50):
        """
        n_qubits: number of qubits (4–8 practical; more = more expressive)
        n_iters:  optimisation iterations for boundary learning
        """
        self.n_qubits  = n_qubits
        self.n_iters   = n_iters
        self.n_params  = n_qubits * 2  # 2 Ry layers

        self.params_normal  : Optional[np.ndarray] = None
        self.params_anomaly : Optional[np.ndarray] = None
        self.threshold       : Optional[float]     = None
        self.fitted          : bool                = False

        self.simulator = None
        self.hamiltonian = None
        if QISKIT_AVAILABLE:
            try:
                self.simulator  = AerSimulator(method='statevector', device='GPU')
            except Exception:
                self.simulator  = AerSimulator(method='statevector')
            self.hamiltonian = _build_hamiltonian(n_qubits)
            print(f"VariationalAnomalyDetector: {n_qubits} qubits, "
                  f"{self.n_params} params")
        else:
            print("VariationalAnomalyDetector: Qiskit unavailable — "
                  "classical variance threshold will be used")

    def _features_from_frame(self, frame: np.ndarray) -> np.ndarray:
        """
        Compress a 52-element phase frame into n_qubits features.
        Each feature = mean phase difference in a sub-band, normalised to [0, π].
        """
        phases  = np.unwrap(np.array(frame, dtype=np.float64))
        diffs   = np.abs(np.arctan2(np.sin(np.diff(phases)), np.cos(np.diff(phases))))
        bin_sz  = max(1, len(diffs) // self.n_qubits)
        feats   = np.array([np.mean(diffs[i * bin_sz:(i + 1) * bin_sz])
                            for i in range(self.n_qubits)])
        return feats

    def _mean_energy(self, params: np.ndarray, frames: List[np.ndarray]) -> float:
        """Average Hamiltonian energy over a set of frames."""
        energies = [
            _expectation(params, self._features_from_frame(f),
                          self.n_qubits, self.hamiltonian, self.simulator)
            for f in frames
        ]
        return float(np.mean(energies))

    def fit(self,
            normal_frames: List[np.ndarray],
            anomaly_frames: List[np.ndarray],
            seed: Optional[int] = 42) -> "VariationalAnomalyDetector":
        """
        Learn variational parameters for normal and anomaly classes.

        Optimises params_normal to minimise energy on normal frames
        (ground state), and params_anomaly to maximise energy on
        anomaly frames (excited state).  The threshold is set midway.

        Args:
            normal_frames:  List of (52,) phase arrays representing normal CSI.
            anomaly_frames: List of (52,) phase arrays representing anomalous CSI.
            seed:           Random seed for initial parameter sampling. Pass None
                            for non-reproducible (production) runs.
        """
        if not QISKIT_AVAILABLE or not normal_frames or not anomaly_frames:
            self._fit_classical(normal_frames, anomaly_frames)
            return self

        print("Fitting variational anomaly detector...")
        rng = np.random.default_rng(seed)

        # Minimise energy for normal class
        p0_norm = rng.uniform(0, np.pi, self.n_params)
        res_norm = minimize(
            lambda p: self._mean_energy(p, normal_frames),
            p0_norm,
            method='COBYLA',
            options={'maxiter': self.n_iters, 'rhobeg': 0.5}
        )
        self.params_normal = res_norm.x
        e_normal = res_norm.fun

        # Minimise NEGATIVE energy for anomaly class (maximise energy)
        p0_anom = rng.uniform(0, np.pi, self.n_params)
        res_anom = minimize(
            lambda p: -self._mean_energy(p, anomaly_frames),
            p0_anom,
            method='COBYLA',
            options={'maxiter': self.n_iters, 'rhobeg': 0.5}
        )
        self.params_anomaly = res_anom.x
        e_anomaly = -res_anom.fun

        self.threshold = (e_normal + e_anomaly) / 2.0
        self.fitted    = True
        print(f"  Energy normal:  {e_normal:.4f}")
        print(f"  Energy anomaly: {e_anomaly:.4f}")
        print(f"  Threshold:      {self.threshold:.4f}")
        return self

    def _fit_classical(self, normal_frames: List[np.ndarray],
                        anomaly_frames: List[np.ndarray]) -> None:
        """Classical fallback: threshold = midpoint of variance distributions."""
        def mean_variance(frames):
            variances = [float(np.var(np.unwrap(f))) for f in frames]
            return float(np.mean(variances)) if variances else 0.0

        v_normal  = mean_variance(normal_frames)
        v_anomaly = mean_variance(anomaly_frames)
        self.threshold = (v_normal + v_anomaly) / 2.0
        self.fitted    = True
        print(f"Classical fallback threshold: {self.threshold:.6f}")

    def predict(self, frame: np.ndarray) -> str:
        """
        Classify a single phase frame as "normal" or "anomaly".

        Returns "anomaly" if the frame's Hamiltonian energy exceeds
        the learned threshold; "normal" otherwise.
        """
        if not self.fitted:
            raise RuntimeError("Call fit() before predict()")

        if QISKIT_AVAILABLE and self.params_normal is not None:
            feats  = self._features_from_frame(frame)
            energy = _expectation(self.params_normal, feats,
                                   self.n_qubits, self.hamiltonian, self.simulator)
        else:
            energy = float(np.var(np.unwrap(np.array(frame, dtype=np.float64))))

        return "anomaly" if energy > self.threshold else "normal"

    def predict_proba(self, frame: np.ndarray) -> float:
        """
        Returns a score in [0, 1] indicating anomaly probability.
        Uses a sigmoid centred on the threshold.
        """
        if not self.fitted:
            raise RuntimeError("Call fit() before predict_proba()")

        if QISKIT_AVAILABLE and self.params_normal is not None:
            feats  = self._features_from_frame(frame)
            energy = _expectation(self.params_normal, feats,
                                   self.n_qubits, self.hamiltonian, self.simulator)
        else:
            energy = float(np.var(np.unwrap(np.array(frame, dtype=np.float64))))

        return float(1.0 / (1.0 + np.exp(-(energy - self.threshold))))


# ------------------------------------------------------------------ #
#  Test / demo                                                         #
# ------------------------------------------------------------------ #

if __name__ == "__main__":
    print("VariationalAnomalyDetector — self-test")

    rng = np.random.default_rng(0)
    # Normal: smooth phase progression
    normal  = [np.linspace(0, 0.4, 52) + rng.normal(0, 0.02, 52)
               for _ in range(20)]
    # Anomaly: random noise
    anomaly = [rng.uniform(-0.5, 0.5, 52) for _ in range(20)]

    vad = VariationalAnomalyDetector(n_qubits=4, n_iters=30)
    vad.fit(normal, anomaly)

    # Test 10 normal + 10 anomaly frames
    correct = 0
    total   = 0
    for f in normal[:5]:
        label = vad.predict(f)
        correct += (label == "normal")
        total   += 1
    for f in anomaly[:5]:
        label = vad.predict(f)
        correct += (label == "anomaly")
        total   += 1

    print(f"Accuracy: {correct}/{total} = {correct/total:.0%}")
