#!/usr/bin/env python3
"""
transport_manager.py
Transport selection helper for CSI/control traffic.

Priority:
  1) UDP (Wi-Fi)
  2) BLE GATT
  3) USB CDC serial
"""

from __future__ import annotations

import socket
from dataclasses import dataclass
from typing import Optional, Sequence


@dataclass
class TransportAvailability:
    udp: bool = True
    ble: bool = False
    serial: bool = False


class TransportManager:
    def __init__(self, priority: Sequence[str] = ("udp", "ble", "serial")):
        self.priority = tuple(priority)
        self.active_transport: str = "none"

    def select_transport(self, availability: TransportAvailability) -> str:
        for name in self.priority:
            if name == "udp" and availability.udp:
                self.active_transport = "udp"
                return self.active_transport
            if name == "ble" and availability.ble:
                self.active_transport = "ble"
                return self.active_transport
            if name == "serial" and availability.serial:
                self.active_transport = "serial"
                return self.active_transport
        self.active_transport = "none"
        return self.active_transport

    def send(
        self,
        payload: bytes,
        *,
        udp_target: Optional[tuple[str, int]] = None,
        ble_sender=None,
        serial_writer=None,
    ) -> bool:
        if not payload:
            return False

        if self.active_transport == "udp" and udp_target:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                sock.sendto(payload, udp_target)
                return True

        if self.active_transport == "ble" and ble_sender:
            ble_sender(payload)
            return True

        if self.active_transport == "serial" and serial_writer:
            serial_writer(payload)
            return True

        return False
