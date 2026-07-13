package com.buddhaswatch.companion;

import android.content.Context;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.TextView;

import java.util.List;

/**
 * DeviceListAdapter — displays discovered BLE devices in a ListView.
 */
public class DeviceListAdapter extends ArrayAdapter<BleDevice> {

    public DeviceListAdapter(List<BleDevice> items) {
        super(null, android.R.layout.simple_list_item_2, items);
    }

    public void setItems(List<BleDevice> items) {
        clear();
        addAll(items);
        notifyDataSetChanged();
    }

    public void addItem(BleDevice device) {
        add(device);
        notifyDataSetChanged();
    }

    @Override
    public View getView(int position, View convertView, ViewGroup parent) {
        if (convertView == null) {
            convertView = LayoutInflater.from(parent.getContext())
                .inflate(android.R.layout.simple_list_item_2, parent, false);
        }
        BleDevice device = getItem(position);
        if (device != null) {
            TextView t1 = convertView.findViewById(android.R.id.text1);
            TextView t2 = convertView.findViewById(android.R.id.text2);
            t1.setText(device.name);
            t2.setText(device.address + "  " + device.rssi + " dBm");
        }
        return convertView;
    }
}
