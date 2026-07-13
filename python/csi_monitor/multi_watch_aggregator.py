#!/usr/bin/env python3
"""
multi_watch_aggregator.py
Aggregate CSI packets from multiple watches with transport failover support.
"""

from __future__ import annotations

import json
import socket
from collections import defaultdict, deque
from typing import Callable, Optional

try:
    from python.tools.transport_manager import TransportAvailability, TransportManager
except ImportError:
    from tools.transport_manager import TransportAvailability, TransportManager


class MultiWatchAggregator:
    def __init__(self, udp_port: int = 5500):
        self.udp_port = udp_port
        self.transport = TransportManager()
        self.buffers = defaultdict(lambda: deque(maxlen=4096))
        self.on_packet: Optional[Callable[[dict], None]] = None

    def select_transport(self, udp_ok: bool, ble_ok: bool, serial_ok: bool) -> str:
        return self.transport.select_transport(
            TransportAvailability(udp=udp_ok, ble=ble_ok, serial=serial_ok)
        )

    def ingest_packet(self, packet: bytes) -> Optional[dict]:
        try:
            parsed = json.loads(packet.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return None

        node_id = parsed.get("node_id")
        if not node_id:
            return None

        self.buffers[node_id].append(parsed)
        if self.on_packet:
            self.on_packet(parsed)
        return parsed

    def listen_udp(self):
        self.select_transport(udp_ok=True, ble_ok=False, serial_ok=False)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.bind(("0.0.0.0", self.udp_port))
            while True:
                data, _ = sock.recvfrom(4096)
                self.ingest_packet(data)
