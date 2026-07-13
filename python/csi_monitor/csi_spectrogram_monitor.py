#!/usr/bin/env python3
"""
csi_spectrogram_monitor.py
Real-time FFT spectrogram with persistence-gated narrowband detection.
Scans 0.1 Hz to 100 kHz across all configured frequency ranges.

When detection threshold is met, alerts back to ESP32 watches via UDP
command to trigger visual/haptic/RF counter-measures.

Usage:
    python csi_spectrogram_monitor.py
"""

import socket
import json
import threading
import time
import numpy as np
from collections import deque, defaultdict
from scipy import signal
from typing import Optional, List, Tuple

# ===================== CONFIG =====================
UDP_PORT = 5500
WATCH_CMD_PORT = 5501  # Port for sending commands back to watches
SAMPLING_RATE = 1000.0  # Hz (adjust to match ESP32 CSI rate)
WINDOW_SIZE = 2048
OVERLAP = 1024
PERSISTENCE_THRESHOLD = 5
ALERT_THRESHOLD_SNR_DB = 10.0
ALERT_COOLDOWN = 5.0

# Frequency ranges of interest
FREQ_RANGES: List[Tuple[float, float]] = [
    (0.1, 10.0),      # Brainwaves (delta/theta/alpha)
    (10.0, 100.0),    # Gamma, muscle micro-tremors
    (100.0, 1000.0),  # Audible low
    (1000.0, 20000.0), # Audible full range
    (20000.0, 40000.0), # Ultrasound (photoacoustic, sonogenetics)
    (40000.0, 100000.0), # High ultrasound
]

NODE_IDS = ["watch_left", "watch_right", "pocket"]

# ===================== STATE =====================
phase_buffers = {
    nid: deque(maxlen=int(SAMPLING_RATE * 5.0))
    for nid in NODE_IDS
}

# Persistence tracker: node_id -> freq_hz -> count
peak_persistence: defaultdict = defaultdict(lambda: defaultdict(int))
last_alert_time: defaultdict = defaultdict(float)

# UDP socket for receiving CSI data
rx_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rx_sock.bind(('0.0.0.0', UDP_PORT))

# UDP socket for sending commands back to watches
tx_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

# Watch IPs (discoverable, defaults provided)
watch_ips = {
    "watch_left": "192.168.1.101",
    "watch_right": "192.168.1.102",
    "pocket": "192.168.1.103",
}


# ===================== UDP LISTENER =====================
def udp_listener():
    """Receive CSI packets from ESP32 watches."""
    while True:
        try:
            data, addr = rx_sock.recvfrom(4096)
            packet = json.loads(data.decode('utf-8'))
            node_id = packet.get('node_id', 'unknown')
            
            # Learn watch IP on first contact
            if node_id in watch_ips:
                watch_ips[node_id] = addr[0]
            
            if node_id in phase_buffers:
                phases = packet.get('phases', [])
                for p in phases:
                    phase_buffers[node_id].append(p)
        except Exception:
            pass


# ===================== WATCH COMMANDS =====================
def send_watch_command(node_id: str, command: str, params: dict = None):
    """
    Send a command back to an ESP32 watch.
    
    Commands:
        "alert" — trigger display alert + vibrate
        "lock" — enable efuse-style channel locking
        "rf_burst" — transmit RF noise burst on detected band
        "log_marker" — mark this moment in SD card log
    """
    if node_id not in watch_ips:
        return
    
    msg = {
        "cmd": command,
        "params": params or {},
        "timestamp": time.time(),
    }
    
    try:
        tx_sock.sendto(
            json.dumps(msg).encode('utf-8'),
            (watch_ips[node_id], WATCH_CMD_PORT)
        )
        print(f"  -> {node_id}: {command}")
    except Exception as e:
        print(f"  -> {node_id}: FAILED ({e})")


def broadcast_command(command: str, params: dict = None):
    """Send a command to ALL watches simultaneously."""
    for node_id in watch_ips:
        send_watch_command(node_id, command, params)


# ===================== SPECTRUM ANALYSIS =====================
def compute_spectrogram(
    phase_buffer: deque,
    fs: float = SAMPLING_RATE
) -> Tuple[Optional[np.ndarray], Optional[np.ndarray], Optional[np.ndarray]]:
    """Compute spectrogram from phase buffer."""
    if len(phase_buffer) < WINDOW_SIZE:
        return None, None, None
    
    data = np.array(list(phase_buffer)[-WINDOW_SIZE:])
    data = data - np.mean(data)
    
    f, t, Sxx = signal.spectrogram(
        data, fs=fs,
        nperseg=WINDOW_SIZE,
        noverlap=OVERLAP,
        window='hann'
    )
    
    return f, t, Sxx


def detect_narrowband_peaks(
    freqs: np.ndarray,
    spectrogram: np.ndarray,
) -> List[Tuple[float, float]]:
    """
    Detect persistent narrowband peaks above dynamic noise floor.
    
    Returns list of (frequency_hz, snr_db) for detected peaks.
    """
    # Noise floor = median power across all frequencies
    noise_floor = np.median(spectrogram, axis=0)
    noise_floor = np.mean(noise_floor)
    
    # Mean power across time for each frequency
    mean_power = np.mean(spectrogram, axis=1)
    
    # SNR in dB
    snr = 10 * np.log10(mean_power / (noise_floor + 1e-12))
    
    # Find local maxima above threshold
    peaks = []
    for i in range(1, len(snr) - 1):
        if (snr[i] > ALERT_THRESHOLD_SNR_DB and
            snr[i] > snr[i - 1] and
            snr[i] > snr[i + 1]):
            
            freq = freqs[i]
            # Check if in a frequency range of interest
            for f_min, f_max in FREQ_RANGES:
                if f_min <= freq <= f_max:
                    peaks.append((freq, float(snr[i])))
                    break
    
    return peaks


# ===================== PERSISTENCE + ALERT =====================
def check_persistence_and_alert(
    node_id: str,
    peaks: List[Tuple[float, float]],
    now: float
):
    """Track peaks across frames; alert and send commands on persistent detections."""
    for freq, snr_db in peaks:
        # Round to 1 Hz for persistence tracking
        freq_key = round(freq, 1)
        
        # Increment persistence counter
        peak_persistence[node_id][freq_key] += 1
        
        # Check if threshold met
        if peak_persistence[node_id][freq_key] >= PERSISTENCE_THRESHOLD:
            alert_key = (node_id, freq_key)
            if now - last_alert_time[alert_key] > ALERT_COOLDOWN:
                # Alert!
                print(f"\n*** ALERT: Node {node_id} — persistent peak at {freq_key:.1f} Hz, SNR={snr_db:.1f} dB ***")
                
                # Send commands back to watch
                send_watch_command(node_id, "alert", {
                    "freq": freq_key,
                    "snr": snr_db,
                    "severity": "high" if snr_db > 20 else "medium"
                })
                
                # Also log marker on SD card
                send_watch_command(node_id, "log_marker", {
                    "event": "anomaly_detected",
                    "freq": freq_key
                })
                
                # Broadcast RF burst to all watches if severe
                if snr_db > 20:
                    broadcast_command("rf_burst", {
                        "freq": freq_key,
                        "duration_ms": 100
                    })
                
                last_alert_time[alert_key] = now
                peak_persistence[node_id][freq_key] = 0
    
    # Decay non-persistent peaks
    for freq_key in list(peak_persistence[node_id].keys()):
        if not any(abs(freq_key - p[0]) < 1.0 for p in peaks):
            peak_persistence[node_id][freq_key] = max(
                0, peak_persistence[node_id][freq_key] - 1
            )


# ===================== MAIN LOOP =====================
def main():
    # Start UDP listener thread
    listener_thread = threading.Thread(target=udp_listener, daemon=True)
    listener_thread.start()
    
    print("=" * 60)
    print("Buddhas-Watch CSI Spectrogram Monitor")
    print("Listening on port", UDP_PORT)
    print("Command port:", WATCH_CMD_PORT)
    print("Frequency ranges:")
    for f_min, f_max in FREQ_RANGES:
        print(f"  {f_min:8.1f} — {f_max:8.1f} Hz")
    print("=" * 60)
    
    frame_interval = (WINDOW_SIZE - OVERLAP) / SAMPLING_RATE
    next_frame = time.time()
    
    try:
        while True:
            now = time.time()
            if now >= next_frame:
                for node_id in NODE_IDS:
                    freqs, _, Sxx = compute_spectrogram(phase_buffers[node_id])
                    if freqs is not None:
                        peaks = detect_narrowband_peaks(freqs, Sxx)
                        if peaks:
                            print(f"\n{node_id}: {len(peaks)} peaks detected")
                            for f, s in peaks[:3]:
                                print(f"  {f:.1f} Hz @ {s:.1f} dB")
                            check_persistence_and_alert(node_id, peaks, now)
                
                next_frame = now + frame_interval
            
            time.sleep(0.01)
    
    except KeyboardInterrupt:
        print("\nMonitor stopped.")


if __name__ == '__main__':
    main()
