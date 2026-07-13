package com.buddhaswatch.companion;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanFilter;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;

import androidx.lifecycle.LiveData;
import androidx.lifecycle.MutableLiveData;
import androidx.lifecycle.ViewModel;

import java.util.ArrayList;
import java.util.List;
import java.util.UUID;

/**
 * MainViewModel — manages BLE scanning and the list of discovered Buddhas-Watch devices.
 */
public class MainViewModel extends ViewModel {

    /** BLE service UUID for Buddhas-Watch: 0000AB00-0000-1000-8000-00805F9B34FB */
    public static final UUID CSI_SERVICE_UUID =
        UUID.fromString("0000ab00-0000-1000-8000-00805f9b34fb");

    private static final long SCAN_PERIOD_MS = 10_000;

    private final MutableLiveData<String>      mScanStatus = new MutableLiveData<>("Idle");
    private final MutableLiveData<List<BleDevice>> mDevices = new MutableLiveData<>(new ArrayList<>());
    private DeviceListAdapter mAdapter;

    private BluetoothLeScanner mScanner;
    private Handler            mHandler;
    private boolean            mScanning = false;

    public MainViewModel() {
        mHandler = new Handler(Looper.getMainLooper());
        mAdapter = new DeviceListAdapter(new ArrayList<>());
    }

    public LiveData<String>          getScanStatus() { return mScanStatus; }
    public LiveData<List<BleDevice>> getDevices()    { return mDevices;    }
    public DeviceListAdapter         getDeviceAdapter() { return mAdapter; }

    public void startBleScan(Context ctx) {
        BluetoothAdapter btAdapter = BluetoothAdapter.getDefaultAdapter();
        if (btAdapter == null || !btAdapter.isEnabled()) {
            mScanStatus.setValue("Bluetooth not available or disabled");
            return;
        }

        mScanner = btAdapter.getBluetoothLeScanner();
        if (mScanner == null) {
            mScanStatus.setValue("BLE scanner unavailable");
            return;
        }

        if (mScanning) return;

        /* Filter for devices advertising the CSI service UUID */
        ScanFilter filter = new ScanFilter.Builder()
            .setServiceUuid(new ParcelUuid(CSI_SERVICE_UUID))
            .build();
        ScanSettings settings = new ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build();

        List<ScanFilter> filters = new ArrayList<>();
        filters.add(filter);

        /* Clear previous results */
        List<BleDevice> current = new ArrayList<>();
        mDevices.setValue(current);
        mAdapter.setItems(current);

        mScanning = true;
        mScanStatus.setValue("Scanning for Buddhas-Watch devices...");
        mScanner.startScan(filters, settings, mScanCallback);

        /* Auto-stop after SCAN_PERIOD_MS */
        mHandler.postDelayed(this::stopBleScan, SCAN_PERIOD_MS);
    }

    public void stopBleScan() {
        if (mScanning && mScanner != null) {
            mScanner.stopScan(mScanCallback);
            mScanning = false;
            List<BleDevice> devices = mDevices.getValue();
            int count = devices != null ? devices.size() : 0;
            mScanStatus.setValue("Scan complete — " + count + " device(s) found");
        }
    }

    private final ScanCallback mScanCallback = new ScanCallback() {
        @Override
        public void onScanResult(int callbackType, ScanResult result) {
            BluetoothDevice device = result.getDevice();
            String name = device.getName();
            if (name == null) name = "BuddhasWatch";

            BleDevice bd = new BleDevice(name, device.getAddress(), result.getRssi());

            List<BleDevice> current = mDevices.getValue();
            if (current == null) current = new ArrayList<>();

            /* Avoid duplicates */
            for (BleDevice d : current) {
                if (d.address.equals(bd.address)) return;
            }
            current.add(bd);
            mDevices.postValue(current);
            mAdapter.addItem(bd);
        }

        @Override
        public void onScanFailed(int errorCode) {
            mScanStatus.postValue("Scan failed: error " + errorCode);
            mScanning = false;
        }
    };

    @Override
    protected void onCleared() {
        super.onCleared();
        stopBleScan();
    }
}
