import unittest

from python.tools.transport_manager import TransportAvailability, TransportManager


class TransportManagerTests(unittest.TestCase):
    def test_prefers_udp_when_available(self):
        manager = TransportManager()
        selected = manager.select_transport(TransportAvailability(udp=True, ble=True, serial=True))
        self.assertEqual(selected, "udp")

    def test_falls_back_to_ble(self):
        manager = TransportManager()
        selected = manager.select_transport(TransportAvailability(udp=False, ble=True, serial=True))
        self.assertEqual(selected, "ble")

    def test_falls_back_to_serial_when_needed(self):
        manager = TransportManager()
        selected = manager.select_transport(TransportAvailability(udp=False, ble=False, serial=True))
        self.assertEqual(selected, "serial")

    def test_returns_none_when_no_transport_available(self):
        manager = TransportManager()
        selected = manager.select_transport(TransportAvailability(udp=False, ble=False, serial=False))
        self.assertEqual(selected, "none")


if __name__ == "__main__":
    unittest.main()
