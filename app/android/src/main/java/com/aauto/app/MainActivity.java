package com.aauto.app;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.ServiceConnection;
import android.os.Bundle;
import android.os.IBinder;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.ListView;
import android.widget.TextView;

import com.aauto.app.core.AaSessionService;
import com.aauto.app.core.AaSessionService.DeviceEntry;
import com.aauto.app.core.AaSessionService.DeviceState;

import java.util.ArrayList;
import java.util.List;

/**
 * Device list screen — launcher entry point.
 *
 * Binds to AaSessionService to read the current device list.
 * Refreshes the list on ACTION_DEVICE_LIST_CHANGED broadcasts.
 *
 * Tapping a CONNECTED device → starts AaDisplayActivity (VideoFocusGain via surface lifecycle).
 * Tapping a RUNNING device   → brings AaDisplayActivity back to front.
 * Long-pressing a device     → disconnects it (calls service.disconnectDevice).
 */
public class MainActivity extends Activity {

    private static final String TAG = "AA.MainActivity";

    private ListView      deviceList_;
    private DeviceAdapter adapter_;

    // ─── Service binding ─────────────────────────────────────────────────────

    private AaSessionService service_ = null;
    private boolean          bound_   = false;

    private final ServiceConnection serviceConnection_ = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName name, IBinder binder) {
            service_ = ((AaSessionService.LocalBinder) binder).getService();
            bound_   = true;
            Log.i(TAG, "AaSessionService connected");
            refreshDeviceList();
        }

        @Override
        public void onServiceDisconnected(ComponentName name) {
            service_ = null;
            bound_   = false;
        }
    };

    // ─── Device list change receiver ─────────────────────────────────────────

    private final BroadcastReceiver listChangedReceiver_ = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            refreshDeviceList();
        }
    };

    // ─── Activity lifecycle ───────────────────────────────────────────────────

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_device_list);

        deviceList_ = findViewById(R.id.device_list);
        adapter_    = new DeviceAdapter(this, new ArrayList<>());
        deviceList_.setAdapter(adapter_);

        deviceList_.setOnItemClickListener((parent, view, position, id) -> {
            DeviceEntry item = adapter_.getItem(position);
            if (item != null) onDeviceSelected(item);
        });

        deviceList_.setOnItemLongClickListener((parent, view, position, id) -> {
            DeviceEntry item = adapter_.getItem(position);
            if (item != null) onDeviceLongPress(item);
            return true;
        });

        Log.i(TAG, "onCreate");
    }

    @Override
    protected void onStart() {
        super.onStart();
        // Bind to AaSessionService (starts it if not running)
        Intent serviceIntent = new Intent(this, AaSessionService.class);
        startForegroundService(serviceIntent);
        bindService(serviceIntent, serviceConnection_, Context.BIND_AUTO_CREATE);
        registerReceiver(listChangedReceiver_,
            new IntentFilter(AaSessionService.ACTION_DEVICE_LIST_CHANGED));
    }

    @Override
    protected void onStop() {
        super.onStop();
        unregisterReceiver(listChangedReceiver_);
        if (bound_) {
            unbindService(serviceConnection_);
            bound_   = false;
            service_ = null;
        }
    }

    // ─── Device list ─────────────────────────────────────────────────────────

    private void refreshDeviceList() {
        List<DeviceEntry> entries = (bound_ && service_ != null)
            ? service_.getDeviceList()
            : new ArrayList<>();

        adapter_.clear();
        adapter_.addAll(entries);
        adapter_.notifyDataSetChanged();
    }

    // ─── Device interactions ─────────────────────────────────────────────────

    private void onDeviceSelected(DeviceEntry item) {
        Log.i(TAG, "Device selected: " + item.deviceId + " state=" + item.state);
        Intent intent = new Intent(this, AaDisplayActivity.class);
        intent.putExtra(AaDisplayActivity.EXTRA_DEVICE_ID, item.deviceId);
        if (item.state == DeviceState.RUNNING) {
            // Session is already streaming — bring the existing display to front.
            intent.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
        } else {
            // Session is CONNECTED — open the display; surface lifecycle triggers VideoFocusGain.
            intent.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        }
        startActivity(intent);
    }

    private void onDeviceLongPress(DeviceEntry item) {
        Log.i(TAG, "Long press: disconnecting " + item.deviceId);
        if (bound_ && service_ != null) {
            service_.disconnectDevice(item.deviceId);
        }
    }

    // ─── List adapter ────────────────────────────────────────────────────────

    private static class DeviceAdapter extends ArrayAdapter<DeviceEntry> {

        DeviceAdapter(Context context, List<DeviceEntry> items) {
            super(context, R.layout.item_device, items);
        }

        @Override
        public View getView(int position, View convertView, ViewGroup parent) {
            if (convertView == null) {
                convertView = LayoutInflater.from(getContext())
                    .inflate(R.layout.item_device, parent, false);
            }
            DeviceEntry item = getItem(position);
            if (item == null) return convertView;

            TextView nameView   = convertView.findViewById(R.id.device_name);
            TextView infoView   = convertView.findViewById(R.id.device_info);
            TextView statusView = convertView.findViewById(R.id.device_status);

            nameView.setText(item.displayName);
            infoView.setText(item.isWireless ? "Wireless" : "USB");

            if (item.state == DeviceState.RUNNING) {
                statusView.setVisibility(View.VISIBLE);
                statusView.setText("Running");
                convertView.setBackgroundColor(0xFF1a2e3a);
            } else if (item.state == DeviceState.CONNECTED) {
                statusView.setVisibility(View.VISIBLE);
                statusView.setText("Connected");
                convertView.setBackgroundColor(0xFF1e2e1e);
            } else {
                statusView.setVisibility(View.GONE);
                convertView.setBackgroundColor(0x00000000);
            }

            return convertView;
        }
    }
}
