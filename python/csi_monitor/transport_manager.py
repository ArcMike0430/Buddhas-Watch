#!/usr/bin/env python3
"""
Transport manager for CSI packet intake on compute nodes.

Supports:
  - Wi-Fi: UDP JSON packets
  - BLE: TCP line-delimited JSON packets (e.g., BLE gateway bridge)
  - Cable: USB serial line-delimited JSON packets
"""

import json
import socket
from typing import Dict, List, Optional, Tuple


def extract_phase_samples(packet: Dict) -> List[float]:
    """Normalize CSI phase payload from packet variants."""
    if "phases" in packet:
        raw = packet["phases"]
    else:
        raw = packet.get("phase", [])

    if isinstance(raw, (int, float)):
        raw = [raw]
    elif raw is None:
        raw = []

    phases: List[float] = []
    for value in raw:
        try:
            phases.append(float(value))
        except (TypeError, ValueError):
            continue
    return phases


class CSITransportManager:
    """Receives CSI packets from Wi-Fi, BLE bridge, or USB cable."""

    def __init__(
        self,
        transport: str = "wifi",
        host: str = "0.0.0.0",
        udp_port: int = 5500,
        ble_port: int = 5502,
        serial_port: str = "/dev/ttyUSB0",
        serial_baudrate: int = 115200,
        timeout: float = 1.0,
    ):
        self.transport = transport.lower()
        self.host = host
        self.udp_port = udp_port
        self.ble_port = ble_port
        self.serial_port = serial_port
        self.serial_baudrate = serial_baudrate
        self.timeout = timeout

        self._udp_sock: Optional[socket.socket] = None
        self._ble_server: Optional[socket.socket] = None
        self._ble_client: Optional[socket.socket] = None
        self._serial = None

    def open(self):
        if self.transport == "wifi":
            self._udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self._udp_sock.bind((self.host, self.udp_port))
            self._udp_sock.settimeout(self.timeout)
            return

        if self.transport == "ble":
            self._ble_server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self._ble_server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self._ble_server.bind((self.host, self.ble_port))
            self._ble_server.listen(1)
            self._ble_server.settimeout(self.timeout)
            return

        if self.transport == "cable":
            try:
                import serial  # type: ignore
            except ImportError as exc:
                raise RuntimeError(
                    "Cable transport requires pyserial (`pip install pyserial`)."
                ) from exc
            self._serial = serial.Serial(self.serial_port, self.serial_baudrate, timeout=self.timeout)
            return

        raise ValueError(f"Unsupported transport '{self.transport}'. Use wifi, ble, or cable.")

    def close(self):
        if self._udp_sock:
            self._udp_sock.close()
            self._udp_sock = None
        if self._ble_client:
            self._ble_client.close()
            self._ble_client = None
        if self._ble_server:
            self._ble_server.close()
            self._ble_server = None
        if self._serial:
            self._serial.close()
            self._serial = None

    def _decode_packet(self, payload: bytes) -> Optional[Dict]:
        try:
            packet = json.loads(payload.decode("utf-8").strip())
            if not isinstance(packet, dict):
                return None
            packet.setdefault("node_id", "unknown")
            packet["phases"] = extract_phase_samples(packet)
            return packet
        except (UnicodeDecodeError, json.JSONDecodeError):
            return None

    def receive_packet(self) -> Tuple[Optional[Dict], Optional[str]]:
        """Receive one normalized packet and source identifier."""
        if self.transport == "wifi" and self._udp_sock:
            data, addr = self._udp_sock.recvfrom(4096)
            return self._decode_packet(data), addr[0]

        if self.transport == "ble" and self._ble_server:
            if self._ble_client is None:
                self._ble_client, addr = self._ble_server.accept()
                self._ble_client.settimeout(self.timeout)
                return None, addr[0]
            data = self._ble_client.recv(4096)
            if not data:
                self._ble_client.close()
                self._ble_client = None
                return None, None
            first_line = data.splitlines()[0] if data.splitlines() else data
            return self._decode_packet(first_line), "ble-client"

        if self.transport == "cable" and self._serial:
            line = self._serial.readline()
            if not line:
                return None, None
            return self._decode_packet(line), self.serial_port

        return None, None
