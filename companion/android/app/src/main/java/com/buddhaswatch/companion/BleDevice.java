package com.buddhaswatch.companion;

/**
 * BleDevice — a discovered Buddhas-Watch BLE device.
 */
public class BleDevice {
    public final String name;
    public final String address;
    public final int    rssi;

    public BleDevice(String name, String address, int rssi) {
        this.name    = name;
        this.address = address;
        this.rssi    = rssi;
    }

    @Override
    public String toString() {
        return name + " (" + address + ") " + rssi + " dBm";
    }
}
