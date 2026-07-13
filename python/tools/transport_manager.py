#!/usr/bin/env python3
"""
transport_manager.py — Unified CSI transport layer for Buddhas-Watch

Abstracts the three data transport mechanisms so that all analysis monitors
(phase variance, coherence, spectrogram, quantum) can receive CSI data from
any source without modification:

  Transport.UDP  — JSON over UDP port 5500 (WiFi from watch or companion app)
  Transport.BLE  — BLE GATT notifications decoded into CsiPacket objects
  Transport.USB  — JSON-Lines over USB serial at high baud rate

Usage:
  from python.tools.transport_manager import TransportManager, Transport

  mgr = TransportManager(transports=[Transport.UDP, Transport.BLE])
  mgr.start()
  while True:
      pkt = mgr.get(timeout=2.0)
      if pkt:
          analyse(pkt.phases)
"""

import json
import logging
import queue
import socket
import struct
import threading
import time
from enum import Enum, auto
from typing import Callable, Dict, List, Optional

logger = logging.getLogger(__name__)


# ── Transport modes ────────────────────────────────────────────────────────────
class Transport(Enum):
    UDP = auto()
    BLE = auto()
    USB = auto()


# ── Data model ────────────────────────────────────────────────────────────────
class CsiPacket:
    """Normalised CSI measurement frame from any transport source."""

    __slots__ = ("node_id", "timestamp", "channel", "rssi", "rate",
                 "phases", "magnitudes", "source", "recv_time")

    def __init__(self, data: dict, source: Transport = Transport.UDP):
        self.node_id    = str(data.get("node_id",    "unknown"))
        self.timestamp  = int(data.get("timestamp",  0))
        self.channel    = int(data.get("channel",    0))
        self.rssi       = int(data.get("rssi",       0))
        self.rate       = int(data.get("rate",       0))
        self.phases     = list(data.get("phases",     []))
        self.magnitudes = list(data.get("magnitudes", []))
        self.source     = source
        self.recv_time  = time.time()

    def to_dict(self) -> dict:
        return {
            "node_id":    self.node_id,
            "timestamp":  self.timestamp,
            "channel":    self.channel,
            "rssi":       self.rssi,
            "rate":       self.rate,
            "phases":     self.phases,
            "magnitudes": self.magnitudes,
        }


# ── UDP transport ─────────────────────────────────────────────────────────────
class UdpTransport(threading.Thread):
    """
    Listens on a UDP port for JSON-Lines CSI packets.
    Compatible with the existing watch firmware (UDP port 5500).
    """

    def __init__(self, out_q: queue.Queue, host: str = "0.0.0.0",
                 port: int = 5500, buf_size: int = 4096):
        super().__init__(daemon=True, name="udp-transport")
        self._q        = out_q
        self._host     = host
        self._port     = port
        self._buf_size = buf_size
        self._running  = False
        self._sock: Optional[socket.socket] = None

    def run(self):
        self._running = True
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.settimeout(1.0)
        try:
            self._sock.bind((self._host, self._port))
            logger.info("UDP transport: listening on %s:%d", self._host, self._port)
            while self._running:
                try:
                    raw, _addr = self._sock.recvfrom(self._buf_size)
                    try:
                        d = json.loads(raw.decode("utf-8"))
                        self._q.put_nowait(CsiPacket(d, Transport.UDP))
                    except (json.JSONDecodeError, ValueError):
                        pass
                except socket.timeout:
                    continue
        except OSError as e:
            logger.error("UDP bind failed: %s", e)
        finally:
            self._sock.close()

    def stop(self):
        self._running = False


# ── USB serial transport ───────────────────────────────────────────────────────
class UsbTransport(threading.Thread):
    """
    Reads JSON-Lines CSI packets from a USB serial port (UART over USB).

    The watch firmware can be configured to mirror CSI packets to UART
    at 921600 baud in addition to (or instead of) UDP streaming.
    """

    def __init__(self, out_q: queue.Queue, port: str = "/dev/ttyUSB0",
                 baud: int = 921600):
        super().__init__(daemon=True, name="usb-transport")
        self._q       = out_q
        self._port    = port
        self._baud    = baud
        self._running = False

    def run(self):
        self._running = True
        try:
            import serial  # pyserial
        except ImportError:
            logger.warning("pyserial not installed — USB transport disabled")
            return

        try:
            ser = serial.Serial(self._port, self._baud, timeout=1.0)
            logger.info("USB transport: %s @ %d baud", self._port, self._baud)
            while self._running:
                try:
                    line = ser.readline().decode("utf-8", errors="ignore").strip()
                    if line.startswith("{"):
                        d = json.loads(line)
                        self._q.put_nowait(CsiPacket(d, Transport.USB))
                except (json.JSONDecodeError, ValueError):
                    pass
            ser.close()
        except Exception as e:
            logger.error("USB transport error: %s", e)

    def stop(self):
        self._running = False


# ── BLE transport ─────────────────────────────────────────────────────────────
class BleTransport:
    """
    Connects to a Buddhas-Watch via BLE and receives CSI notifications.
    Uses the `bleak` library (async; run in a separate thread with its own event loop).

    BLE is optional — this class is a no-op if bleak is not installed.
    """

    BLE_SERVICE_UUID = "0000ab00-0000-1000-8000-00805f9b34fb"
    BLE_META_UUID    = "0000ab01-0000-1000-8000-00805f9b34fb"
    BLE_DATA_UUID    = "0000ab02-0000-1000-8000-00805f9b34fb"
    BLE_CTRL_UUID    = "0000ab03-0000-1000-8000-00805f9b34fb"

    def __init__(self, out_q: queue.Queue, device_address: Optional[str] = None):
        self._q       = out_q
        self._address = device_address
        self._thread: Optional[threading.Thread] = None
        self._running = False
        # Chunk reassembly state
        self._chunks: Dict[int, bytes] = {}
        self._total_chunks = 0
        self._meta: dict    = {}

    def start(self):
        self._running = True
        self._thread  = threading.Thread(target=self._run_async,
                                          daemon=True, name="ble-transport")
        self._thread.start()

    def stop(self):
        self._running = False

    def _run_async(self):
        try:
            import asyncio
            loop = asyncio.new_event_loop()
            loop.run_until_complete(self._async_main())
        except ImportError:
            logger.warning("asyncio not available")
        except Exception as e:
            logger.error("BLE transport error: %s", e)

    async def _async_main(self):
        try:
            from bleak import BleakScanner, BleakClient
        except ImportError:
            logger.warning("bleak not installed — BLE transport disabled")
            return

        address = self._address
        if not address:
            logger.info("BLE: scanning for Buddhas-Watch…")
            devices = await BleakScanner.discover(
                timeout=10.0, service_uuids=[self.BLE_SERVICE_UUID])
            if not devices:
                logger.warning("BLE: no devices found")
                return
            address = devices[0].address
            logger.info("BLE: connecting to %s", address)

        async with BleakClient(address) as client:
            await client.start_notify(self.BLE_META_UUID, self._on_metadata)
            await client.start_notify(self.BLE_DATA_UUID, self._on_data)
            await client.write_gatt_char(self.BLE_CTRL_UUID, bytes([0x01]))
            logger.info("BLE: streaming CSI from %s", address)
            while self._running:
                await __import__("asyncio").sleep(0.1)
            await client.write_gatt_char(self.BLE_CTRL_UUID, bytes([0x00]))

    def _on_metadata(self, _handle, data: bytearray):
        if len(data) < 12:
            return
        self._meta = {
            "channel":   data[0],
            "rssi":      struct.unpack_from("b", data, 1)[0],
            "rate":      struct.unpack_from("<H", data, 2)[0],
            "timestamp": struct.unpack_from("<Q", data, 4)[0],
            "node_id":   "ble_watch",
        }

    def _on_data(self, _handle, data: bytearray):
        if len(data) < 2:
            return
        chunk_idx    = data[0]
        total_chunks = data[1]
        payload      = bytes(data[2:])

        if chunk_idx == 0:
            self._chunks       = {}
            self._total_chunks = total_chunks

        self._chunks[chunk_idx] = payload

        if len(self._chunks) >= self._total_chunks:
            magnitudes, phases = [], []
            for ci in range(self._total_chunks):
                chunk = self._chunks.get(ci, b"")
                for j in range(0, len(chunk) - 1, 2):
                    mv = chunk[j]     / 255.0 * 64.0
                    pv = chunk[j + 1] / 255.0 * 2.0 * 3.14159 - 3.14159
                    magnitudes.append(mv)
                    phases.append(pv)
            d = dict(self._meta)
            d["magnitudes"] = magnitudes
            d["phases"]     = phases
            self._q.put_nowait(CsiPacket(d, Transport.BLE))


# ── Manager ────────────────────────────────────────────────────────────────────
class TransportManager:
    """
    Unified manager that starts one or more transports and provides a single
    packet queue.

    Example:
        mgr = TransportManager(transports=[Transport.UDP, Transport.BLE])
        mgr.start()
        pkt = mgr.get(timeout=1.0)
    """

    def __init__(self,
                 transports: Optional[List[Transport]] = None,
                 udp_host:   str = "0.0.0.0",
                 udp_port:   int = 5500,
                 usb_port:   str = "/dev/ttyUSB0",
                 usb_baud:   int = 921600,
                 ble_address: Optional[str] = None,
                 on_packet:   Optional[Callable[["CsiPacket"], None]] = None,
                 queue_size:  int = 1000):

        if transports is None:
            transports = [Transport.UDP]

        self._transports = transports
        self._q          = queue.Queue(maxsize=queue_size)
        self._on_packet  = on_packet
        self._workers: List = []

        if Transport.UDP in transports:
            self._workers.append(UdpTransport(self._q, udp_host, udp_port))

        if Transport.USB in transports:
            self._workers.append(UsbTransport(self._q, usb_port, usb_baud))

        if Transport.BLE in transports:
            self._workers.append(BleTransport(self._q, ble_address))

        # Optional callback thread
        if on_packet:
            self._cb_thread = threading.Thread(target=self._callback_loop,
                                                daemon=True, name="transport-cb")
        else:
            self._cb_thread = None

    def start(self):
        """Start all configured transport workers."""
        for w in self._workers:
            if hasattr(w, "start"):
                w.start()
        if self._cb_thread:
            self._cb_thread.start()
        logger.info("TransportManager started with: %s",
                    [t.name for t in self._transports])

    def stop(self):
        """Stop all workers."""
        for w in self._workers:
            if hasattr(w, "stop"):
                w.stop()
        logger.info("TransportManager stopped")

    def get(self, timeout: float = 1.0) -> Optional[CsiPacket]:
        """
        Blocking get with timeout.  Returns None on timeout.
        """
        try:
            return self._q.get(timeout=timeout)
        except queue.Empty:
            return None

    def qsize(self) -> int:
        return self._q.qsize()

    @property
    def active_transports(self) -> List[Transport]:
        return list(self._transports)

    def _callback_loop(self):
        while True:
            pkt = self.get(timeout=1.0)
            if pkt and self._on_packet:
                try:
                    self._on_packet(pkt)
                except Exception as e:
                    logger.error("on_packet callback error: %s", e)
