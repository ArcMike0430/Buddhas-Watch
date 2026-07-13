"""
test_transport_manager.py — Unit tests for python.tools.transport_manager

Run with:
    python -m unittest python.tests.test_transport_manager -v
    python -m compileall python
"""

import json
import queue
import socket
import time
import threading
import unittest
from typing import Optional

from python.tools.transport_manager import (
    CsiPacket,
    Transport,
    TransportManager,
    UdpTransport,
    UsbTransport,
    BleTransport,
)


# ── CsiPacket ─────────────────────────────────────────────────────────────────
class TestCsiPacket(unittest.TestCase):

    SAMPLE = {
        "node_id":    "watch_01",
        "timestamp":  1234567890,
        "channel":    6,
        "rssi":       -45,
        "rate":       54,
        "phases":     [0.1, 0.2, 0.3],
        "magnitudes": [10.0, 12.0, 8.5],
    }

    def test_construction_from_dict(self):
        pkt = CsiPacket(self.SAMPLE)
        self.assertEqual(pkt.node_id,  "watch_01")
        self.assertEqual(pkt.channel,  6)
        self.assertEqual(pkt.rssi,     -45)
        self.assertAlmostEqual(pkt.phases[1], 0.2)
        self.assertEqual(pkt.source,   Transport.UDP)

    def test_construction_with_source(self):
        pkt = CsiPacket(self.SAMPLE, source=Transport.BLE)
        self.assertEqual(pkt.source, Transport.BLE)

    def test_missing_fields_use_defaults(self):
        pkt = CsiPacket({})
        self.assertEqual(pkt.node_id,    "unknown")
        self.assertEqual(pkt.channel,    0)
        self.assertEqual(pkt.phases,     [])
        self.assertEqual(pkt.magnitudes, [])

    def test_to_dict_round_trip(self):
        pkt = CsiPacket(self.SAMPLE)
        d   = pkt.to_dict()
        self.assertEqual(d["node_id"], "watch_01")
        self.assertEqual(d["phases"],  [0.1, 0.2, 0.3])

    def test_recv_time_is_recent(self):
        before = time.time()
        pkt    = CsiPacket(self.SAMPLE)
        after  = time.time()
        self.assertGreaterEqual(pkt.recv_time, before)
        self.assertLessEqual(pkt.recv_time, after)


# ── UdpTransport ──────────────────────────────────────────────────────────────
class TestUdpTransport(unittest.TestCase):
    """Send a real UDP packet and verify the transport delivers it."""

    PORT = 15500  # Use a non-standard port to avoid conflicts

    def _send_packet(self, data: dict, port: Optional[int] = None):
        """Helper: send one JSON UDP packet to localhost."""
        dest_port = port if port is not None else self.PORT
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.sendto(json.dumps(data).encode(), ("127.0.0.1", dest_port))

    def test_receives_udp_packet(self):
        q   = queue.Queue()
        rx  = UdpTransport(q, host="127.0.0.1", port=self.PORT)
        rx.start()
        time.sleep(0.1)  # Allow socket to bind

        payload = {
            "node_id":    "test_node",
            "timestamp":  999,
            "channel":    1,
            "rssi":       -60,
            "rate":       54,
            "phases":     [1.0, 2.0],
            "magnitudes": [5.0, 6.0],
        }
        self._send_packet(payload)

        try:
            pkt = q.get(timeout=2.0)
        finally:
            rx.stop()

        self.assertIsNotNone(pkt)
        self.assertEqual(pkt.node_id,  "test_node")
        self.assertEqual(pkt.channel,  1)
        self.assertEqual(pkt.source,   Transport.UDP)

    def test_invalid_json_is_ignored(self):
        q   = queue.Queue()
        rx  = UdpTransport(q, host="127.0.0.1", port=self.PORT + 1)
        rx.start()
        time.sleep(0.1)

        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.sendto(b"not json at all", ("127.0.0.1", self.PORT + 1))

        time.sleep(0.2)
        rx.stop()
        self.assertTrue(q.empty(), "Invalid JSON should not produce a packet")

    def test_multiple_packets(self):
        PORT = self.PORT + 2
        q   = queue.Queue()
        rx  = UdpTransport(q, host="127.0.0.1", port=PORT)
        rx.start()
        time.sleep(0.1)

        for i in range(5):
            self._send_packet({"node_id": f"node_{i}", "channel": i}, port=PORT)
            time.sleep(0.02)

        rx.stop()
        time.sleep(0.2)

        count = 0
        while not q.empty():
            q.get_nowait()
            count += 1
        self.assertEqual(count, 5)


# ── TransportManager ──────────────────────────────────────────────────────────
class TestTransportManager(unittest.TestCase):

    PORT = 15510

    def test_udp_transport_selected(self):
        mgr = TransportManager(transports=[Transport.UDP], udp_port=self.PORT)
        self.assertIn(Transport.UDP, mgr.active_transports)
        self.assertNotIn(Transport.BLE, mgr.active_transports)

    def test_ble_transport_selected(self):
        mgr = TransportManager(transports=[Transport.BLE])
        self.assertIn(Transport.BLE, mgr.active_transports)

    def test_usb_transport_selected(self):
        mgr = TransportManager(transports=[Transport.USB])
        self.assertIn(Transport.USB, mgr.active_transports)

    def test_multiple_transports(self):
        mgr = TransportManager(transports=[Transport.UDP, Transport.USB],
                                udp_port=self.PORT + 1)
        transports = mgr.active_transports
        self.assertIn(Transport.UDP, transports)
        self.assertIn(Transport.USB, transports)
        self.assertNotIn(Transport.BLE, transports)

    def test_default_is_udp(self):
        mgr = TransportManager()
        self.assertEqual(mgr.active_transports, [Transport.UDP])

    def test_get_returns_none_on_timeout(self):
        mgr = TransportManager(transports=[Transport.UDP], udp_port=self.PORT + 2)
        mgr.start()
        pkt = mgr.get(timeout=0.1)
        mgr.stop()
        self.assertIsNone(pkt)

    def test_on_packet_callback(self):
        received = []
        PORT     = self.PORT + 3

        def cb(pkt):
            received.append(pkt)

        mgr = TransportManager(transports=[Transport.UDP],
                                udp_port=PORT, on_packet=cb)
        mgr.start()
        time.sleep(0.15)

        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            payload = json.dumps({"node_id": "cb_test", "channel": 11}).encode()
            s.sendto(payload, ("127.0.0.1", PORT))

        time.sleep(0.3)
        mgr.stop()

        self.assertEqual(len(received), 1)
        self.assertEqual(received[0].node_id, "cb_test")

    def test_qsize_increases_with_packets(self):
        PORT = self.PORT + 4
        mgr  = TransportManager(transports=[Transport.UDP], udp_port=PORT)
        mgr.start()
        time.sleep(0.1)

        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            for _ in range(3):
                s.sendto(json.dumps({"node_id": "q_test"}).encode(),
                         ("127.0.0.1", PORT))
                time.sleep(0.02)

        time.sleep(0.2)
        mgr.stop()
        self.assertGreaterEqual(mgr.qsize(), 1)

    def test_start_stop_is_idempotent(self):
        """Manager should not crash if stopped without being started."""
        mgr = TransportManager(transports=[Transport.UDP], udp_port=self.PORT + 5)
        mgr.stop()  # Should not raise


# ── BleTransport (unit-level, without actual hardware) ───────────────────────
class TestBleTransport(unittest.TestCase):

    def test_stop_before_start_does_not_crash(self):
        q   = queue.Queue()
        ble = BleTransport(q)
        ble.stop()  # Should be safe

    def test_metadata_decoding(self):
        q   = queue.Queue()
        ble = BleTransport(q)

        import struct
        channel = 6
        rssi    = -45
        rate    = 54
        ts      = 1234567890

        data = bytearray(20)
        data[0] = channel
        struct.pack_into("b",  data, 1, rssi)
        struct.pack_into("<H", data, 2, rate)
        struct.pack_into("<Q", data, 4, ts)

        ble._on_metadata(None, data)
        self.assertEqual(ble._meta["channel"],   channel)
        self.assertEqual(ble._meta["rssi"],      rssi)
        self.assertEqual(ble._meta["timestamp"], ts)

    def test_data_chunk_reassembly(self):
        q   = queue.Queue()
        ble = BleTransport(q)
        ble._meta = {"node_id": "ble_watch", "channel": 6, "rssi": -40,
                     "rate": 54, "timestamp": 0}

        # Single chunk (chunk_idx=0, total=1), 4 mag/phase pairs
        payload = bytearray([
            0,    # chunk_idx
            1,    # total_chunks
            100, 128,   # mag/phase pair 0
            50,  64,    # mag/phase pair 1
            200, 200,   # mag/phase pair 2
            10,  10,    # mag/phase pair 3
        ])

        ble._on_data(None, payload)

        pkt = q.get_nowait()
        self.assertIsNotNone(pkt)
        self.assertEqual(len(pkt.magnitudes), 4)
        self.assertEqual(len(pkt.phases),     4)
        self.assertEqual(pkt.source, Transport.BLE)


if __name__ == "__main__":
    unittest.main()
