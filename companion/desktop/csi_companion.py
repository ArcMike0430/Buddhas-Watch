#!/usr/bin/env python3
"""
csi_companion.py — Buddhas-Watch Desktop/Laptop Companion App

Cross-platform (Windows / macOS / Linux / Steam Deck) companion application
for the Buddhas-Watch ESP32-S3 CSI monitor.

Features:
  - WiFi UDP listener for real-time CSI streams (port 5500)
  - BLE connection via `bleak` library (async BLE for all platforms)
  - Live CSI magnitude/phase visualization (matplotlib + tkinter)
  - Multi-node dashboard: up to 8 simultaneous watch nodes
  - Record / playback CSI sessions
  - Export to CSV, JSON-Lines, or HDF5
  - FFT analysis of phase variance
  - Integration with the existing Python monitoring stack (UDP port 5500)

Usage:
  python csi_companion.py [--udp-port 5500] [--ble] [--no-gui]

Requirements:
  pip install -r requirements.txt

Distribution:
  - Windows / macOS / Linux: GitHub Releases (executable built with PyInstaller)
  - Steam Deck: AppImage or Flatpak (see companion/README.md)
"""

import argparse
import asyncio
import json
import queue
import socket
import struct
import sys
import threading
import time
from collections import deque
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# ── Optional imports (graceful degradation) ──────────────────────────────────
try:
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox
    HAS_TK = True
except ImportError:
    HAS_TK = False

try:
    import matplotlib
    matplotlib.use("TkAgg" if HAS_TK else "Agg")
    import matplotlib.pyplot as plt
    from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
    import matplotlib.animation as animation
    HAS_MPL = True
except ImportError:
    HAS_MPL = False

try:
    import numpy as np
    HAS_NP = True
except ImportError:
    HAS_NP = False

try:
    import bleak
    from bleak import BleakScanner, BleakClient
    HAS_BLEAK = True
except ImportError:
    HAS_BLEAK = False

try:
    import h5py
    HAS_H5 = True
except ImportError:
    HAS_H5 = False

# ── Constants ─────────────────────────────────────────────────────────────────
UDP_PORT        = 5500
MAX_SUBCARRIERS = 52
MAX_HISTORY     = 500
BLE_SERVICE_UUID    = "0000ab00-0000-1000-8000-00805f9b34fb"
BLE_META_UUID       = "0000ab01-0000-1000-8000-00805f9b34fb"
BLE_DATA_UUID       = "0000ab02-0000-1000-8000-00805f9b34fb"
BLE_CTRL_UUID       = "0000ab03-0000-1000-8000-00805f9b34fb"

APP_NAME    = "Buddhas-Watch Companion"
APP_VERSION = "1.0.0"

# ── Data structures ───────────────────────────────────────────────────────────
class CsiPacket:
    """Single CSI measurement frame from a watch node."""
    __slots__ = ("node_id", "timestamp", "channel", "rssi", "rate",
                 "phases", "magnitudes", "recv_time")

    def __init__(self, data: dict):
        self.node_id    = data.get("node_id",    "unknown")
        self.timestamp  = data.get("timestamp",  0)
        self.channel    = data.get("channel",    0)
        self.rssi       = data.get("rssi",       0)
        self.rate       = data.get("rate",       0)
        self.phases     = data.get("phases",     [])
        self.magnitudes = data.get("magnitudes", [])
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


# ── UDP receiver thread ───────────────────────────────────────────────────────
class UdpReceiver(threading.Thread):
    """Listens on UDP_PORT for JSON CSI packets from all watch nodes."""

    def __init__(self, packet_queue: queue.Queue, port: int = UDP_PORT):
        super().__init__(daemon=True, name="udp-receiver")
        self._queue   = packet_queue
        self._port    = port
        self._running = False

    def run(self):
        self._running = True
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.settimeout(1.0)
            sock.bind(("0.0.0.0", self._port))
            print(f"[UDP] Listening on :{self._port}")
            while self._running:
                try:
                    data, addr = sock.recvfrom(4096)
                    try:
                        d = json.loads(data.decode("utf-8"))
                        self._queue.put(CsiPacket(d))
                    except json.JSONDecodeError:
                        pass
                except socket.timeout:
                    continue
                except Exception as e:
                    print(f"[UDP] Error: {e}")
                    break

    def stop(self):
        self._running = False


# ── BLE receiver (async) ──────────────────────────────────────────────────────
class BleReceiver:
    """Connects to a Buddhas-Watch via BLE and receives CSI notifications."""

    def __init__(self, packet_queue: queue.Queue):
        self._queue  = packet_queue
        self._client: Optional[BleakClient] = None
        # BLE chunk reassembly state
        self._chunks: Dict[int, bytes] = {}
        self._total_chunks = 0
        self._meta: dict = {}

    async def scan_and_connect(self, timeout: float = 10.0) -> Optional[str]:
        """Scan for Buddhas-Watch devices and return the address of the first found."""
        if not HAS_BLEAK:
            print("[BLE] bleak not installed — run: pip install bleak")
            return None

        print("[BLE] Scanning for Buddhas-Watch devices...")
        devices = await BleakScanner.discover(timeout=timeout,
                                               service_uuids=[BLE_SERVICE_UUID])
        for d in devices:
            print(f"[BLE] Found: {d.name} ({d.address})")
            if d.name and "BuddhasWatch" in d.name:
                return d.address
        if devices:
            # Take the first one that advertises the service UUID
            return devices[0].address
        return None

    async def connect(self, address: str):
        if not HAS_BLEAK:
            return
        self._client = BleakClient(address)
        await self._client.connect()
        print(f"[BLE] Connected to {address}")

        # Subscribe to metadata and data characteristics
        await self._client.start_notify(BLE_META_UUID, self._on_metadata)
        await self._client.start_notify(BLE_DATA_UUID, self._on_data)

        # Send start command
        await self._client.write_gatt_char(BLE_CTRL_UUID, bytes([0x01]))
        print("[BLE] CSI capture started")

    def _on_metadata(self, _handle, data: bytearray):
        """Decode 20-byte metadata characteristic."""
        if len(data) < 12:
            return
        channel  = data[0]
        rssi     = struct.unpack_from("b", data, 1)[0]
        rate     = struct.unpack_from("<H", data, 2)[0]
        ts       = struct.unpack_from("<Q", data, 4)[0]
        self._meta = {"channel": channel, "rssi": rssi, "rate": rate,
                      "timestamp": ts, "node_id": "ble_watch"}

    def _on_data(self, _handle, data: bytearray):
        """Reassemble chunked CSI data notification."""
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
            # All chunks received — reassemble
            magnitudes = []
            phases     = []
            for ci in range(self._total_chunks):
                chunk = self._chunks.get(ci, b"")
                for j in range(0, len(chunk) - 1, 2):
                    mv = chunk[j]     / 255.0 * 64.0  # scale back
                    pv = chunk[j + 1] / 255.0 * 2 * 3.14159 - 3.14159
                    magnitudes.append(mv)
                    phases.append(pv)

            d = dict(self._meta)
            d["magnitudes"] = magnitudes
            d["phases"]     = phases
            self._queue.put(CsiPacket(d))

    async def disconnect(self):
        if self._client and self._client.is_connected:
            await self._client.write_gatt_char(BLE_CTRL_UUID, bytes([0x00]))
            await self._client.disconnect()
            print("[BLE] Disconnected")


# ── Recording / export ────────────────────────────────────────────────────────
class Recorder:
    def __init__(self):
        self._history: List[CsiPacket] = []
        self._recording = False

    def start(self):
        self._history.clear()
        self._recording = True

    def stop(self):
        self._recording = False

    def record(self, pkt: CsiPacket):
        if self._recording:
            self._history.append(pkt)

    @property
    def count(self) -> int:
        return len(self._history)

    def export_jsonl(self, path: str):
        with open(path, "w") as f:
            for pkt in self._history:
                f.write(json.dumps(pkt.to_dict()) + "\n")
        print(f"[Export] {len(self._history)} packets → {path}")

    def export_csv(self, path: str):
        if not self._history:
            return
        with open(path, "w") as f:
            # Header
            n = len(self._history[0].magnitudes)
            mag_hdrs   = ",".join(f"mag_{i}"   for i in range(n))
            phase_hdrs = ",".join(f"phase_{i}" for i in range(n))
            f.write(f"node_id,timestamp,channel,rssi,rate,{mag_hdrs},{phase_hdrs}\n")
            for pkt in self._history:
                mags   = ",".join(f"{v:.4f}" for v in pkt.magnitudes)
                phases = ",".join(f"{v:.4f}" for v in pkt.phases)
                f.write(f"{pkt.node_id},{pkt.timestamp},{pkt.channel},"
                        f"{pkt.rssi},{pkt.rate},{mags},{phases}\n")
        print(f"[Export] CSV → {path}")

    def export_hdf5(self, path: str):
        if not HAS_H5:
            print("[Export] h5py not installed — skipping HDF5 export")
            return
        if not HAS_NP:
            print("[Export] numpy not installed — skipping HDF5 export")
            return
        import numpy as np
        with h5py.File(path, "w") as f:
            n = len(self._history)
            mags   = np.array([p.magnitudes for p in self._history], dtype=np.float32)
            phases = np.array([p.phases     for p in self._history], dtype=np.float32)
            ts     = np.array([p.timestamp  for p in self._history], dtype=np.int64)
            rssi   = np.array([p.rssi       for p in self._history], dtype=np.int8)
            ch     = np.array([p.channel    for p in self._history], dtype=np.uint8)
            f.create_dataset("magnitudes", data=mags)
            f.create_dataset("phases",     data=phases)
            f.create_dataset("timestamps", data=ts)
            f.create_dataset("rssi",       data=rssi)
            f.create_dataset("channels",   data=ch)
            f.attrs["n_packets"]   = n
            f.attrs["app_version"] = APP_VERSION
        print(f"[Export] HDF5 → {path} ({len(self._history)} packets)")


# ── GUI application ───────────────────────────────────────────────────────────
class CompanionApp:
    """Main tkinter + matplotlib GUI for the Buddhas-Watch companion."""

    def __init__(self, root: "tk.Tk", args):
        self._root      = root
        self._args      = args
        self._q         = queue.Queue(maxsize=500)
        self._nodes:    Dict[str, deque] = {}  # node_id → deque of CsiPacket
        self._recorder  = Recorder()
        self._udp_rx    = UdpReceiver(self._q, port=args.udp_port)
        self._ble_rx    = BleReceiver(self._q) if HAS_BLEAK else None
        self._ble_loop  = None
        self._selected_node = tk.StringVar(value="")

        root.title(f"{APP_NAME} v{APP_VERSION}")
        root.geometry("1024x700")
        root.protocol("WM_DELETE_WINDOW", self._on_close)

        self._build_ui()
        self._udp_rx.start()
        self._poll_queue()

    def _build_ui(self):
        # ── Top toolbar ──────────────────────────────────────────────────────
        toolbar = ttk.Frame(self._root)
        toolbar.pack(side=tk.TOP, fill=tk.X, padx=5, pady=3)

        ttk.Label(toolbar, text="Node:").pack(side=tk.LEFT)
        self._node_combo = ttk.Combobox(toolbar, textvariable=self._selected_node,
                                         width=20)
        self._node_combo.pack(side=tk.LEFT, padx=4)
        self._node_combo.bind("<<ComboboxSelected>>", lambda _: self._refresh_chart())

        ttk.Button(toolbar, text="BLE Scan",
                   command=self._start_ble_scan).pack(side=tk.LEFT, padx=2)

        self._rec_btn = ttk.Button(toolbar, text="● Record",
                                    command=self._toggle_record)
        self._rec_btn.pack(side=tk.LEFT, padx=2)

        ttk.Button(toolbar, text="Export…",
                   command=self._export_dialog).pack(side=tk.LEFT, padx=2)

        self._status_var = tk.StringVar(value="Waiting for data…")
        ttk.Label(toolbar, textvariable=self._status_var).pack(side=tk.RIGHT)

        # ── Charts ────────────────────────────────────────────────────────────
        if HAS_MPL:
            self._fig, (self._ax_mag, self._ax_phase) = plt.subplots(2, 1, figsize=(10, 4))
            self._fig.tight_layout(pad=2.0)
            self._canvas = FigureCanvasTkAgg(self._fig, master=self._root)
            self._canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

            x = list(range(MAX_SUBCARRIERS))
            (self._line_mag,)   = self._ax_mag.plot(x, [0]*MAX_SUBCARRIERS,
                                                      "b-", lw=1.2, label="Magnitude")
            (self._line_phase,) = self._ax_phase.plot(x, [0]*MAX_SUBCARRIERS,
                                                        "r-", lw=1.2, label="Phase (rad)")
            self._ax_mag.set_ylabel("Magnitude")
            self._ax_phase.set_ylabel("Phase (rad)")
            self._ax_phase.set_xlabel("Subcarrier index")
            self._ax_mag.legend(loc="upper right")
            self._ax_phase.legend(loc="upper right")
        else:
            ttk.Label(self._root,
                      text="Install matplotlib for charts: pip install matplotlib").pack()

        # ── Stats panel ───────────────────────────────────────────────────────
        stats_frame = ttk.LabelFrame(self._root, text="Statistics")
        stats_frame.pack(fill=tk.X, padx=5, pady=3)
        self._stats_var = tk.StringVar(value="–")
        ttk.Label(stats_frame, textvariable=self._stats_var,
                  font=("Courier", 9)).pack(anchor=tk.W, padx=5)

    def _poll_queue(self):
        """Pull CSI packets from the queue every 50 ms and update the UI."""
        updated = False
        try:
            while True:
                pkt: CsiPacket = self._q.get_nowait()
                self._recorder.record(pkt)

                nid = pkt.node_id
                if nid not in self._nodes:
                    self._nodes[nid] = deque(maxlen=MAX_HISTORY)
                    self._node_combo["values"] = list(self._nodes.keys())
                    if not self._selected_node.get():
                        self._selected_node.set(nid)

                self._nodes[nid].append(pkt)
                updated = True
        except queue.Empty:
            pass

        if updated and HAS_MPL:
            self._refresh_chart()

        self._root.after(50, self._poll_queue)

    def _refresh_chart(self):
        nid = self._selected_node.get()
        if not nid or nid not in self._nodes:
            return
        history = self._nodes[nid]
        if not history:
            return

        latest = history[-1]
        mags   = latest.magnitudes[:MAX_SUBCARRIERS]
        phases = latest.phases[:MAX_SUBCARRIERS]

        if HAS_MPL and hasattr(self, "_line_mag"):
            self._line_mag.set_ydata(mags + [0] * (MAX_SUBCARRIERS - len(mags)))
            self._line_phase.set_ydata(phases + [0] * (MAX_SUBCARRIERS - len(phases)))
            self._ax_mag.relim()
            self._ax_mag.autoscale_view()
            self._ax_phase.relim()
            self._ax_phase.autoscale_view()
            self._canvas.draw_idle()

        self._stats_var.set(
            f"Node: {latest.node_id}  Ch: {latest.channel}  "
            f"RSSI: {latest.rssi} dBm  Rate: {latest.rate}  "
            f"Pkts: {len(history)}")

    def _start_ble_scan(self):
        if not HAS_BLEAK:
            messagebox.showerror("BLE", "Install bleak: pip install bleak")
            return

        self._status_var.set("Scanning BLE…")

        def run_scan():
            loop = asyncio.new_event_loop()
            address = loop.run_until_complete(self._ble_rx.scan_and_connect())
            if address:
                loop.run_until_complete(self._ble_rx.connect(address))
                self._status_var.set(f"BLE connected: {address}")
                # Keep running the async loop for BLE notifications
                loop.run_forever()
            else:
                self._status_var.set("No Buddhas-Watch device found")

        t = threading.Thread(target=run_scan, daemon=True, name="ble-loop")
        t.start()

    def _toggle_record(self):
        if self._recorder._recording:
            self._recorder.stop()
            self._rec_btn.config(text="● Record")
            self._status_var.set(f"Stopped — {self._recorder.count} packets recorded")
        else:
            self._recorder.start()
            self._rec_btn.config(text="■ Stop")
            self._status_var.set("Recording…")

    def _export_dialog(self):
        if self._recorder.count == 0:
            messagebox.showinfo("Export", "No data recorded yet")
            return

        path = filedialog.asksaveasfilename(
            defaultextension=".jsonl",
            filetypes=[
                ("JSON-Lines", "*.jsonl"),
                ("CSV", "*.csv"),
                ("HDF5", "*.h5"),
                ("All files", "*.*"),
            ],
            initialfile=f"csi_{datetime.now().strftime('%Y%m%d_%H%M%S')}",
        )
        if not path:
            return

        if path.endswith(".csv"):
            self._recorder.export_csv(path)
        elif path.endswith(".h5") or path.endswith(".hdf5"):
            self._recorder.export_hdf5(path)
        else:
            self._recorder.export_jsonl(path)

        messagebox.showinfo("Export", f"Exported {self._recorder.count} packets to\n{path}")

    def _on_close(self):
        self._udp_rx.stop()
        self._root.destroy()


# ── CLI / headless mode ───────────────────────────────────────────────────────
def run_headless(args):
    """No-GUI mode: print CSI stats to stdout, optionally log to file."""
    q      = queue.Queue()
    rx     = UdpReceiver(q, port=args.udp_port)
    rx.start()

    out_file = open(args.output, "a") if args.output else None
    print(f"[headless] Listening on UDP :{args.udp_port}  Ctrl-C to stop")
    try:
        while True:
            try:
                pkt: CsiPacket = q.get(timeout=2)
                line = json.dumps(pkt.to_dict())
                print(line)
                if out_file:
                    out_file.write(line + "\n")
                    out_file.flush()
            except queue.Empty:
                continue
    except KeyboardInterrupt:
        print("\nStopped")
    finally:
        rx.stop()
        if out_file:
            out_file.close()


# ── Entry point ───────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(description=f"{APP_NAME} v{APP_VERSION}")
    parser.add_argument("--udp-port", type=int, default=UDP_PORT,
                        help=f"UDP port to listen on (default {UDP_PORT})")
    parser.add_argument("--ble",      action="store_true",
                        help="Auto-scan and connect to nearest BLE watch on startup")
    parser.add_argument("--no-gui",   action="store_true",
                        help="Run in headless mode (print to stdout)")
    parser.add_argument("--output",   type=str, default=None,
                        help="Headless mode: append JSON-Lines to this file")
    args = parser.parse_args()

    if args.no_gui or not HAS_TK:
        run_headless(args)
        return

    root = tk.Tk()
    app  = CompanionApp(root, args)

    if args.ble and HAS_BLEAK:
        root.after(1000, app._start_ble_scan)

    root.mainloop()


if __name__ == "__main__":
    main()
