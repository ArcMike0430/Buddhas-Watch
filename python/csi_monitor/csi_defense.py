#!/usr/bin/env python3
"""
csi_defense.py
Integrated CSI detection + anti-phase emission system.
Monitors phase variance across multiple nodes, emits counter-measures on detection.

Part of Buddhas-Watch — Production-grade distributed CSI collection system.
"""

import threading
import time
import os
import numpy as np
from collections import deque, defaultdict
import sounddevice as sd
from transport_manager import CSITransportManager, extract_phase_samples

# ===================== CONFIG =====================
UDP_PORT = 5500
SAMPLING_RATE = 1000.0
BUFFER_DURATION = 5.0
WINDOW_SIZE = 2048
PERSISTENCE_THRESHOLD = 5
ALERT_THRESHOLD_SNR_DB = 10.0
ALERT_COOLDOWN = 5.0
ANTI_PHASE_DURATION = 3.0
FREQ_RANGES = [(0.1, 100.0), (100.0, 500.0), (500.0, 20000.0), (20000.0, 100000.0)]
NODE_IDS = ["watch_left", "watch_right", "pocket"]

# ===================== STATE =====================
phase_buffers = {nid: deque(maxlen=int(SAMPLING_RATE * BUFFER_DURATION * 2))
                 for nid in NODE_IDS}
peak_persistence = defaultdict(lambda: defaultdict(int))
last_alert = defaultdict(float)

transport_manager = CSITransportManager(
    transport=os.getenv("CSI_INPUT_TRANSPORT", "wifi"),
    udp_port=UDP_PORT,
)
transport_manager.open()

def udp_listener():
    while True:
        try:
            packet, _ = transport_manager.receive_packet()
            if not packet:
                continue
            node_id = packet.get('node_id', 'unknown')
            if node_id in phase_buffers:
                for p in extract_phase_samples(packet):
                    phase_buffers[node_id].append(p)
        except Exception:
            pass

def emit_anti_phase(freq, duration=ANTI_PHASE_DURATION):
    """Emit 180-degree phase-shifted tone at detected frequency."""
    t = np.linspace(0, duration, int(44100 * duration))
    signal = 0.5 * np.sin(2 * np.pi * freq * t + np.pi)
    sd.play(signal, samplerate=44100, blocking=False)
    print(f"ANTI-PHASE EMITTED at {freq:.1f} Hz for {duration}s")

def main():
    threading.Thread(target=udp_listener, daemon=True).start()
    print("CSI Defense System Running... Monitoring nodes:", NODE_IDS)

    while True:
        now = time.time()
        for node_id in phase_buffers:
            buf = phase_buffers[node_id]
            if len(buf) < WINDOW_SIZE:
                continue
            values = np.array(list(buf)[-WINDOW_SIZE:])
            values = values - np.mean(values)
            window = np.hanning(len(values))
            fft_vals = np.fft.rfft(values * window)
            power = np.abs(fft_vals) ** 2 / len(values)
            freqs = np.fft.rfftfreq(len(values), 1 / SAMPLING_RATE)

            noise_floor = np.median(power)
            snr_db = 10 * np.log10(power / (noise_floor + 1e-12))

            for i in range(1, len(snr_db) - 1):
                if snr_db[i] > ALERT_THRESHOLD_SNR_DB and snr_db[i] > snr_db[i-1] and snr_db[i] > snr_db[i+1]:
                    freq = freqs[i]
                    if any(low <= freq <= high for low, high in FREQ_RANGES):
                        peak_persistence[node_id][freq] += 1
                        if peak_persistence[node_id][freq] >= PERSISTENCE_THRESHOLD:
                            key = (node_id, round(freq, 1))
                            if now - last_alert[key] > ALERT_COOLDOWN:
                                print(f"ALERT: Persistent peak at {freq:.1f} Hz on {node_id}")
                                emit_anti_phase(freq)
                                last_alert[key] = now
                            peak_persistence[node_id][freq] = 0
        time.sleep(0.05)

if __name__ == '__main__':
    main()
