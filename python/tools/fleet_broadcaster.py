#!/usr/bin/env python3
"""
fleet_broadcaster.py
Broadcast commands and sync pulses to all ESP32 watches simultaneously.
Used for:
  - Coordinated baseline calibration
  - Simultaneous counter-measure triggers
  - Time sync for cross-watch coherence analysis
  - Mode switching (monitor/log/scan/defend)

Usage:
    # Sync all watches to current time
    python fleet_broadcaster.py sync
    
    # Start baseline calibration on all watches
    python fleet_broadcaster.py calibrate --duration 120
    
    # Emergency silence all counter-measures
    python fleet_broadcaster.py silence
    
    # Set all watches to a specific mode
    python fleet_broadcaster.py mode --name defend
"""

import argparse
import json
import socket
import time
from datetime import datetime


class FleetBroadcaster:
    """Broadcast commands to all ESP32 watches in the fleet."""
    
    def __init__(self, cmd_port: int = 5501):
        self.cmd_port = cmd_port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        
        # Known watches (discovered dynamically from CSI packets)
        self.watches = {
            "watch_left": "192.168.1.101",
            "watch_right": "192.168.1.102",
            "pocket": "192.168.1.103",
        }
    
    def broadcast(self, command: str, params: dict = None):
        """Send a command to every known watch."""
        msg = {
            "cmd": command,
            "params": params or {},
            "timestamp": time.time(),
            "broadcast": True,
        }
        data = json.dumps(msg).encode('utf-8')
        
        for name, ip in self.watches.items():
            try:
                self.sock.sendto(data, (ip, self.cmd_port))
                print(f"  -> {name} ({ip})")
            except Exception as e:
                print(f"  -> {name}: FAILED ({e})")
    
    def sync_time(self):
        """Broadcast current UTC time to sync all watch clocks."""
        now = datetime.utcnow()
        self.broadcast("sync", {
            "utc_epoch_s": time.time(),
            "utc_iso": now.isoformat(),
        })
        print(f"  Time: {now.isoformat()}")
    
    def calibrate(self, duration_s: int = 120):
        """Begin baseline calibration on all watches."""
        self.broadcast("calibrate", {
            "duration_s": duration_s,
        })
    
    def set_mode(self, mode: str):
        """Set operating mode on all watches: monitor, log, scan, defend."""
        valid_modes = ["monitor", "log", "scan", "defend"]
        if mode not in valid_modes:
            print(f"Invalid mode. Choose from: {valid_modes}")
            return
        self.broadcast("mode", {"mode": mode})
    
    def silence(self):
        """Cancel all active counter-measures on all watches."""
        self.broadcast("silence", {})
    
    def shutdown(self):
        """Put all watches into deep sleep."""
        self.broadcast("shutdown", {})


def main():
    parser = argparse.ArgumentParser(
        description="Buddhas-Watch Fleet Broadcaster"
    )
    parser.add_argument('action', nargs='?', default='sync',
                        help='Action: sync, calibrate, mode, silence, shutdown')
    parser.add_argument('--duration', type=int, default=120,
                        help='Duration for calibration (seconds)')
    parser.add_argument('--name', type=str, default='monitor',
                        help='Mode name: monitor, log, scan, defend')
    
    args = parser.parse_args()
    
    fleet = FleetBroadcaster()
    
    print(f"\nFleet Broadcaster — {args.action}")
    print("-" * 40)
    
    if args.action == 'sync':
        fleet.sync_time()
    elif args.action == 'calibrate':
        fleet.calibrate(args.duration)
    elif args.action == 'mode':
        fleet.set_mode(args.name)
    elif args.action == 'silence':
        fleet.silence()
    elif args.action == 'shutdown':
        fleet.shutdown()
    else:
        print(f"Unknown action: {args.action}")
    
    print("-" * 40)
    print("Done.")


if __name__ == '__main__':
    main()
