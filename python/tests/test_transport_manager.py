import json
import socket
import threading
import unittest

from python.csi_monitor.transport_manager import CSITransportManager, extract_phase_samples


class TransportManagerTests(unittest.TestCase):
    def test_extract_phase_samples_supports_phase_and_phases(self):
        self.assertEqual(extract_phase_samples({"phase": 1.25}), [1.25])
        self.assertEqual(extract_phase_samples({"phase": [1, "2.5", "bad"]}), [1.0, 2.5])
        self.assertEqual(extract_phase_samples({"phases": [0.1, 0.2]}), [0.1, 0.2])
        self.assertEqual(extract_phase_samples({"phase": ["bad", "worse"]}), [])

    def test_wifi_transport_receives_and_normalizes_packet(self):
        rx = CSITransportManager(transport="wifi", udp_port=0, timeout=1.0)
        rx.open()
        try:
            port = rx._udp_sock.getsockname()[1]

            def sender():
                tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                payload = json.dumps({"node_id": "watch_left", "phase": [1, 2, 3]}).encode("utf-8")
                tx.sendto(payload, ("127.0.0.1", port))
                tx.close()

            th = threading.Thread(target=sender, daemon=True)
            th.start()

            packet, source = rx.receive_packet()
            self.assertIsNotNone(packet)
            self.assertEqual(source, "127.0.0.1")
            self.assertEqual(packet["node_id"], "watch_left")
            self.assertEqual(packet["phases"], [1.0, 2.0, 3.0])
            th.join(timeout=1.0)
        finally:
            rx.close()


if __name__ == "__main__":
    unittest.main()
