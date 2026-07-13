package com.buddhaswatch.companion;

import android.Manifest;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.view.Menu;
import android.view.MenuItem;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;
import androidx.lifecycle.ViewModelProvider;

import com.buddhaswatch.companion.databinding.ActivityMainBinding;

/**
 * MainActivity — entry point for the Buddhas-Watch companion app.
 *
 * Shows the device list and allows the user to:
 *  - Scan for Buddhas-Watch devices via BLE
 *  - Connect and view live CSI data (BLE or WiFi UDP)
 *  - Configure the watch settings remotely
 *  - Export captured data
 */
public class MainActivity extends AppCompatActivity {

    private static final int PERM_REQUEST_CODE = 100;
    private static final String[] REQUIRED_PERMISSIONS = {
        Manifest.permission.BLUETOOTH_SCAN,
        Manifest.permission.BLUETOOTH_CONNECT,
        Manifest.permission.ACCESS_FINE_LOCATION,
    };

    private ActivityMainBinding mBinding;
    private MainViewModel       mViewModel;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mBinding   = ActivityMainBinding.inflate(getLayoutInflater());
        mViewModel = new ViewModelProvider(this).get(MainViewModel.class);
        setContentView(mBinding.getRoot());
        setSupportActionBar(mBinding.toolbar);

        requestRequiredPermissions();
        setupUi();

        /* Start background CSI receiver service */
        Intent svc = new Intent(this, CsiReceiverService.class);
        ContextCompat.startForegroundService(this, svc);
    }

    private void requestRequiredPermissions() {
        boolean allGranted = true;
        for (String perm : REQUIRED_PERMISSIONS) {
            if (ContextCompat.checkSelfPermission(this, perm)
                    != PackageManager.PERMISSION_GRANTED) {
                allGranted = false;
                break;
            }
        }
        if (!allGranted) {
            ActivityCompat.requestPermissions(this, REQUIRED_PERMISSIONS, PERM_REQUEST_CODE);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode,
                                           String[] permissions,
                                           int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == PERM_REQUEST_CODE) {
            for (int r : grantResults) {
                if (r != PackageManager.PERMISSION_GRANTED) {
                    Toast.makeText(this, "BLE permissions required for device scanning",
                                   Toast.LENGTH_LONG).show();
                    return;
                }
            }
            /* Permissions granted — start BLE scan */
            mViewModel.startBleScan(this);
        }
    }

    private void setupUi() {
        /* Device list: populated by BLE scanner */
        mBinding.deviceList.setAdapter(mViewModel.getDeviceAdapter());
        mBinding.deviceList.setOnItemClickListener((parent, view, pos, id) -> {
            BleDevice device = mViewModel.getDeviceAdapter().getItem(pos);
            if (device != null) {
                Intent intent = new Intent(this, CsiViewerActivity.class);
                intent.putExtra(CsiViewerActivity.EXTRA_DEVICE_ADDRESS, device.address);
                intent.putExtra(CsiViewerActivity.EXTRA_DEVICE_NAME,    device.name);
                startActivity(intent);
            }
        });

        /* BLE scan button */
        mBinding.btnScan.setOnClickListener(v -> mViewModel.startBleScan(this));

        /* WiFi UDP mode button */
        mBinding.btnWifiMode.setOnClickListener(v -> {
            Intent intent = new Intent(this, CsiViewerActivity.class);
            intent.putExtra(CsiViewerActivity.EXTRA_MODE, CsiViewerActivity.MODE_UDP);
            startActivity(intent);
        });

        /* Observe scan status */
        mViewModel.getScanStatus().observe(this, status ->
            mBinding.txtStatus.setText(status));

        /* Observe discovered devices */
        mViewModel.getDevices().observe(this, devices ->
            mViewModel.getDeviceAdapter().notifyDataSetChanged());
    }

    @Override
    public boolean onCreateOptionsMenu(Menu menu) {
        getMenuInflater().inflate(R.menu.main_menu, menu);
        return true;
    }

    @Override
    public boolean onOptionsItemSelected(MenuItem item) {
        if (item.getItemId() == R.id.action_settings) {
            startActivity(new Intent(this, DeviceSettingsActivity.class));
            return true;
        }
        return super.onOptionsItemSelected(item);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        mViewModel.stopBleScan();
    }
}
