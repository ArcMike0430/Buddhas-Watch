#!/usr/bin/env python3
"""
esp32_alert.py
Counter-measures module — sends commands back to ESP32 watches
to trigger visual, haptic, and RF counter-measures on anomaly detection.

Commands sent via UDP to port 5501 on each watch:
  - alert:   Flash display red + vibrate motor
  - rf_burst: Transmit RF noise burst on detected frequency band
  - lock:    Enable channel lock (efuse-style)
  - log_marker: Mark event in SD card log
  - sweep:   Begin frequency sweep on BLE/Wi-Fi to break coherence

Usage:
    from esp32_alert import WatchCommander
    
    cmd = WatchCommander()
    cmd.alert_all(freq=40000, severity="high")
    cmd.rf_burst("watch_left", freq=40000, duration_ms=200)
"""

import json
import socket
import time
from typing import Dict, Optional


class WatchCommander:
    """Sends counter-measure commands to ESP32 watches via UDP."""
    
    def __init__(self, cmd_port: int = 5501, timeout: float = 0.5):
        self.cmd_port = cmd_port
        self.timeout = timeout
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(timeout)
        
        # Watch IPs — discovered automatically from CSI packets
        self.watch_ips: Dict[str, str] = {
            "watch_left": "192.168.1.101",
            "watch_right": "192.168.1.102",
            "pocket": "192.168.1.103",
        }
    
    def update_ip(self, node_id: str, ip: str):
        """Update a watch's IP (called when CSI packet arrives)."""
        self.watch_ips[node_id] = ip
    
    def _send(self, node_id: str, command: str, params: dict = None) -> bool:
        """Send a command to one watch. Returns True if sent."""
        if node_id not in self.watch_ips:
            print(f"  [ESP32] Unknown node: {node_id}")
            return False
        
        msg = {
            "cmd": command,
            "params": params or {},
            "timestamp": time.time(),
        }
        
        try:
            data = json.dumps(msg).encode('utf-8')
            self.sock.sendto(data, (self.watch_ips[node_id], self.cmd_port))
            return True
        except Exception as e:
            print(f"  [ESP32] Send failed: {e}")
            return False
    
    def alert(self, node_id: str, freq: float = 0, severity: str = "medium"):
        """Flash display and vibrate on a specific watch."""
        return self._send(node_id, "alert", {
            "freq": freq,
            "severity": severity,
            "pattern": "pulse" if severity == "high" else "solid"
        })
    
    def alert_all(self, freq: float = 0, severity: str = "medium"):
        """Alert all watches simultaneously."""
        results = []
        for node_id in self.watch_ips:
            results.append(self.alert(node_id, freq, severity))
        return all(results)
    
    def rf_burst(self, node_id: str, freq: float, duration_ms: int = 100):
        """
        Transmit RF noise burst on the detected frequency band.
        
        The ESP32 switches to transmit mode and broadcasts noise
        on the band where the anomaly was detected, disrupting
        coherent interference in the shared RF field.
        """
        return self._send(node_id, "rf_burst", {
            "freq": freq,
            "duration_ms": duration_ms,
            "power": "max"
        })
    
    def rf_burst_all(self, freq: float, duration_ms: int = 100):
        """All watches transmit RF noise simultaneously."""
        results = []
        for node_id in self.watch_ips:
            results.append(self.rf_burst(node_id, freq, duration_ms))
        return all(results)
    
    def lock_channel(self, node_id: str, channel: int = 0):
        """
        Enable channel lock on the ESP32's efuse-style protection.
        
        Locks the current Wi-Fi channel to prevent channel-switching
        attacks or forced deauthentication.
        """
        return self._send(node_id, "lock", {
            "channel": channel
        })
    
    def log_marker(self, node_id: str, event: str, details: dict = None):
        """Write a marker to the watch's SD card log."""
        return self._send(node_id, "log_marker", {
            "event": event,
            "details": details or {},
        })
    
    def begin_sweep(self, node_id: str, 
                     start_freq: float = 2400,
                     end_freq: float = 2500,
                     step_hz: float = 1.0,
                     duration_ms: int = 1000):
        """
        Begin a frequency sweep to break coherent interference.
        
        Sweeps the BLE/Wi-Fi radio across a frequency range,
        disrupting any phase-locked interference pattern.
        """
        return self._send(node_id, "sweep", {
            "start_mhz": start_freq,
            "end_mhz": end_freq,
            "step_hz": step_hz,
            "duration_ms": duration_ms
        })
    
    def silence(self, node_id: str):
        """Cancel all active counter-measures on a watch."""
        return self._send(node_id, "silence", {})
    
    def silence_all(self):
        """Cancel all counter-measures on all watches."""
        results = []
        for node_id in self.watch_ips:
            results.append(self.silence(node_id))
        return all(results)


# ===================== EXAMPLE / TEST =====================
if __name__ == "__main__":
    import time
    
    cmd = WatchCommander()
    
    print("Testing WatchCommander...")
    
    # Alert all watches
    print("\n1. Alert all watches (simulated 40 kHz detection)")
    cmd.alert_all(freq=40000, severity="high")
    
    # RF burst from left watch
    print("\n2. RF burst from left watch")
    cmd.rf_burst("watch_left", freq=40000, duration_ms=200)
    
    # Log marker on all
    print("\n3. Log marker on all watches")
    cmd.log_marker("watch_right", "test_complete", {"test": "initialization"})
    
    print("\nDone.")
