package com.buddhaswatch.companion;

import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothProfile;
import android.content.Context;
import android.os.Bundle;
import android.util.Log;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;
import androidx.lifecycle.ViewModelProvider;

import com.github.mikephil.charting.charts.LineChart;
import com.github.mikephil.charting.data.Entry;
import com.github.mikephil.charting.data.LineData;
import com.github.mikephil.charting.data.LineDataSet;

import java.util.ArrayList;
import java.util.List;
import java.util.UUID;

import com.buddhaswatch.companion.databinding.ActivityCsiViewerBinding;

/**
 * CsiViewerActivity — live CSI magnitude/phase chart from a connected Buddhas-Watch.
 *
 * Supports two modes:
 *   MODE_BLE  — GATT notifications on 0xAB01 (metadata) and 0xAB02 (CSI data)
 *   MODE_UDP  — JSON-Lines received on UDP port 5500 via CsiReceiverService
 */
public class CsiViewerActivity extends AppCompatActivity {

    public static final String EXTRA_DEVICE_ADDRESS = "device_address";
    public static final String EXTRA_DEVICE_NAME    = "device_name";
    public static final String EXTRA_MODE           = "mode";
    public static final int    MODE_BLE             = 0;
    public static final int    MODE_UDP             = 1;

    private static final String TAG = "CsiViewer";

    /** Characteristic UUIDs — must match ble_csi_app.c */
    private static final UUID SERVICE_UUID  = UUID.fromString("0000ab00-0000-1000-8000-00805f9b34fb");
    private static final UUID META_UUID     = UUID.fromString("0000ab01-0000-1000-8000-00805f9b34fb");
    private static final UUID DATA_UUID     = UUID.fromString("0000ab02-0000-1000-8000-00805f9b34fb");
    private static final UUID CTRL_UUID     = UUID.fromString("0000ab03-0000-1000-8000-00805f9b34fb");
    private static final UUID CCCD_UUID     = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");

    private ActivityCsiViewerBinding mBinding;
    private CsiViewerViewModel       mViewModel;
    private BluetoothGatt            mGatt;
    private int                      mMode;

    /* Chart data */
    private LineDataSet mMagDataSet;
    private LineDataSet mPhaseDataSet;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mBinding   = ActivityCsiViewerBinding.inflate(getLayoutInflater());
        mViewModel = new ViewModelProvider(this).get(CsiViewerViewModel.class);
        setContentView(mBinding.getRoot());

        mMode = getIntent().getIntExtra(EXTRA_MODE, MODE_BLE);

        setupChart();

        if (mMode == MODE_BLE) {
            String address = getIntent().getStringExtra(EXTRA_DEVICE_ADDRESS);
            String name    = getIntent().getStringExtra(EXTRA_DEVICE_NAME);
            setTitle("BLE: " + name);
            connectBle(address);
        } else {
            setTitle("WiFi UDP — CSI Stream");
            listenUdp();
        }

        /* Start/stop button */
        mBinding.btnStartStop.setOnClickListener(v -> {
            if (mGatt != null) {
                sendBleControl(mViewModel.isCapturing() ? (byte)0x00 : (byte)0x01);
            }
            mViewModel.toggleCapturing();
            mBinding.btnStartStop.setText(mViewModel.isCapturing() ? "Stop" : "Start");
        });

        /* Export button */
        mBinding.btnExport.setOnClickListener(v -> mViewModel.exportData(this));

        /* Observe incoming CSI packets */
        mViewModel.getLatestPacket().observe(this, this::updateChart);
        mViewModel.getStatsText().observe(this, s -> mBinding.txtStats.setText(s));
    }

    /* ---------------------------------------------------------------------- */
    /*  Chart setup                                                             */
    /* ---------------------------------------------------------------------- */
    private void setupChart() {
        LineChart chart = mBinding.csiChart;

        List<Entry> magEntries   = new ArrayList<>();
        List<Entry> phaseEntries = new ArrayList<>();
        for (int i = 0; i < 52; i++) {
            magEntries.add(new Entry(i, 0));
            phaseEntries.add(new Entry(i, 0));
        }

        mMagDataSet   = new LineDataSet(magEntries,   "Magnitude");
        mPhaseDataSet = new LineDataSet(phaseEntries, "Phase");
        mMagDataSet.setColor(0xFF2196F3);   /* Blue */
        mPhaseDataSet.setColor(0xFFF44336); /* Red  */
        mMagDataSet.setDrawCircles(false);
        mPhaseDataSet.setDrawCircles(false);
        mMagDataSet.setLineWidth(1.5f);
        mPhaseDataSet.setLineWidth(1.5f);

        chart.setData(new LineData(mMagDataSet, mPhaseDataSet));
        chart.getDescription().setEnabled(false);
        chart.setTouchEnabled(true);
        chart.setDragEnabled(true);
        chart.setScaleEnabled(true);
        chart.invalidate();
    }

    private void updateChart(CsiPacket pkt) {
        if (pkt == null) return;
        for (int i = 0; i < Math.min(pkt.magnitudes.length, 52); i++) {
            mMagDataSet.getEntryForIndex(i).setY(pkt.magnitudes[i]);
            mPhaseDataSet.getEntryForIndex(i).setY(pkt.phases[i] * 20f); /* scale for visibility */
        }
        mMagDataSet.notifyDataSetChanged();
        mPhaseDataSet.notifyDataSetChanged();
        mBinding.csiChart.getData().notifyDataChanged();
        mBinding.csiChart.notifyDataSetChanged();
        mBinding.csiChart.invalidate();
    }

    /* ---------------------------------------------------------------------- */
    /*  BLE connection                                                          */
    /* ---------------------------------------------------------------------- */
    private void connectBle(String address) {
        android.bluetooth.BluetoothAdapter adapter = android.bluetooth.BluetoothAdapter.getDefaultAdapter();
        if (adapter == null) { Toast.makeText(this, "No Bluetooth", Toast.LENGTH_SHORT).show(); return; }

        android.bluetooth.BluetoothDevice device = adapter.getRemoteDevice(address);
        mGatt = device.connectGatt(this, false, mGattCallback);
        mBinding.txtStats.setText("Connecting to " + address + "...");
    }

    private void sendBleControl(byte cmd) {
        if (mGatt == null) return;
        BluetoothGattService svc = mGatt.getService(SERVICE_UUID);
        if (svc == null) return;
        BluetoothGattCharacteristic ctrl = svc.getCharacteristic(CTRL_UUID);
        if (ctrl == null) return;
        ctrl.setValue(new byte[]{cmd});
        mGatt.writeCharacteristic(ctrl);
    }

    private void enableNotifications(BluetoothGattCharacteristic ch) {
        mGatt.setCharacteristicNotification(ch, true);
        BluetoothGattDescriptor desc = ch.getDescriptor(CCCD_UUID);
        if (desc != null) {
            desc.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
            mGatt.writeDescriptor(desc);
        }
    }

    private final BluetoothGattCallback mGattCallback = new BluetoothGattCallback() {
        @Override
        public void onConnectionStateChange(BluetoothGatt gatt, int status, int newState) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                Log.i(TAG, "BLE connected — discovering services");
                gatt.discoverServices();
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                Log.i(TAG, "BLE disconnected");
                runOnUiThread(() -> mBinding.txtStats.setText("Disconnected"));
            }
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt gatt, int status) {
            BluetoothGattService svc = gatt.getService(SERVICE_UUID);
            if (svc == null) {
                Log.w(TAG, "CSI service not found");
                return;
            }
            BluetoothGattCharacteristic metaCh = svc.getCharacteristic(META_UUID);
            BluetoothGattCharacteristic dataCh = svc.getCharacteristic(DATA_UUID);
            if (metaCh != null) enableNotifications(metaCh);
            if (dataCh  != null) enableNotifications(dataCh);
            runOnUiThread(() -> mBinding.txtStats.setText("Connected — receiving CSI"));
        }

        @Override
        public void onCharacteristicChanged(BluetoothGatt gatt,
                                             BluetoothGattCharacteristic ch) {
            byte[] data = ch.getValue();
            if (data == null) return;

            if (ch.getUuid().equals(DATA_UUID)) {
                /* Decode chunked CSI data */
                mViewModel.processBleChunk(data);
            } else if (ch.getUuid().equals(META_UUID)) {
                mViewModel.processMetadata(data);
            }
        }
    };

    /* ---------------------------------------------------------------------- */
    /*  WiFi UDP listener                                                       */
    /* ---------------------------------------------------------------------- */
    private void listenUdp() {
        mViewModel.startUdpListener();
        mBinding.txtStats.setText("Listening on UDP :5500...");
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (mGatt != null) {
            mGatt.disconnect();
            mGatt.close();
            mGatt = null;
        }
        mViewModel.stopUdpListener();
    }
}
