#!/usr/bin/env python3
"""
multi_watch_aggregator.py
Aggregate CSI packets from multiple watches with transport failover support.
"""

from __future__ import annotations

import json
import socket
import threading
from collections import defaultdict, deque
from typing import Callable, Optional

try:
    from python.tools.transport_manager import TransportAvailability, TransportManager
except ImportError:
    from tools.transport_manager import TransportAvailability, TransportManager

UDP_PACKET_BYTES = 4096
MAX_BUFFER_SIZE = 4096


class MultiWatchAggregator:
    def __init__(self, udp_port: int = 5500, bind_host: str = "127.0.0.1", packet_bytes: int = UDP_PACKET_BYTES):
        self.udp_port = udp_port
        self.bind_host = bind_host
        self.packet_bytes = packet_bytes
        self.transport = TransportManager()
        self.buffers = defaultdict(lambda: deque(maxlen=MAX_BUFFER_SIZE))
        self.on_packet: Optional[Callable[[dict], None]] = None
        self._stop_event = threading.Event()

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
            sock.bind((self.bind_host, self.udp_port))
            sock.settimeout(0.5)
            while not self._stop_event.is_set():
                try:
                    data, _ = sock.recvfrom(self.packet_bytes)
                except socket.timeout:
                    continue
                self.ingest_packet(data)

    def stop(self):
        self._stop_event.set()
