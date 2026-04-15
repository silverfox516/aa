package com.aauto.app;

import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.ServiceConnection;
import android.net.wifi.WifiConfiguration;
import android.net.wifi.WifiManager;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcelable;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.ListView;
import android.widget.TextView;
import android.widget.Toast;

import com.aauto.app.core.AaSessionService;
import com.aauto.app.core.AaSessionService.SessionEntry;
import com.aauto.app.core.AaSessionService.SessionState;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;

/**
 * Device list screen — launcher entry point.
 *
 * Shows two kinds of entries:
 *   - Live sessions from AaSessionService (CONNECTING / HANDSHAKING / READY / RUNNING)
 *   - Paired Bluetooth devices that are not yet in a session (tap to dial)
 *
 * Tapping a READY/RUNNING session  → activate(handle) + open AaDisplayActivity.
 * Tapping a paired BT entry        → starts wireless dial via WirelessMonitorService.
 * Long-pressing a connected entry  → disconnect.
 */
public class MainActivity extends Activity {

    private static final String TAG = "AA.APP.MainActivity";

    // AAW UUID — kept for reference but not used for filtering (phone only registers
    // it in SDP when the AA app is actively running, so filtering by UUID is unreliable)
    @SuppressWarnings("unused")
    private static final UUID AAW_UUID =
        UUID.fromString("4DE17A00-52CB-11E6-BDF4-0800200C9A66");

    private ListView      deviceList_;
    private DeviceAdapter adapter_;
    private Button        btBluetoothToggle_;
    private Button        btSoftApToggle_;

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

    // ─── Session list change receiver ────────────────────────────────────────

    private final BroadcastReceiver listChangedReceiver_ = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            refreshDeviceList();
        }
    };

    // ─── BT event receiver (debug + bond/SDP refresh) ────────────────────────

    private final BroadcastReceiver btEventReceiver_ = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            String action = intent.getAction();
            BluetoothDevice device = intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE);
            String addr = device != null ? device.getAddress() : "null";
            String name = device != null ? device.getName() : "null";

            switch (action != null ? action : "") {
                case BluetoothAdapter.ACTION_STATE_CHANGED: {
                    int state = intent.getIntExtra(BluetoothAdapter.EXTRA_STATE,
                                                   BluetoothAdapter.ERROR);
                    Log.i(TAG, "BT state changed: " + btStateStr(state));
                    updateBluetoothButton();
                    refreshDeviceList();
                    break;
                }
                case BluetoothDevice.ACTION_BOND_STATE_CHANGED: {
                    int bond = intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE,
                                                  BluetoothDevice.BOND_NONE);
                    Log.i(TAG, "BT bond state: " + addr + " (" + name + ") → " + bondStateStr(bond));
                    if (bond == BluetoothDevice.BOND_BONDED) refreshDeviceList();
                    break;
                }
                case BluetoothDevice.ACTION_ACL_CONNECTED:
                    Log.i(TAG, "BT ACL connected: " + addr + " (" + name + ")");
                    refreshDeviceList();
                    break;
                case BluetoothDevice.ACTION_ACL_DISCONNECTED:
                    Log.i(TAG, "BT ACL disconnected: " + addr + " (" + name + ")");
                    break;
                case BluetoothDevice.ACTION_UUID: {
                    Parcelable[] rawUuids = intent.getParcelableArrayExtra(BluetoothDevice.EXTRA_UUID);
                    StringBuilder sb = new StringBuilder();
                    if (rawUuids != null) {
                        for (Parcelable p : rawUuids) sb.append("\n  ").append(p.toString());
                    } else {
                        sb.append(" (none)");
                    }
                    Log.i(TAG, "BT UUID fetch done: " + addr + " (" + name + ")" + sb);
                    break;
                }
            }
        }
    };

    private static String btStateStr(int state) {
        switch (state) {
            case BluetoothAdapter.STATE_OFF:       return "OFF";
            case BluetoothAdapter.STATE_ON:        return "ON";
            case BluetoothAdapter.STATE_TURNING_ON:  return "TURNING_ON";
            case BluetoothAdapter.STATE_TURNING_OFF: return "TURNING_OFF";
            default: return "UNKNOWN(" + state + ")";
        }
    }

    private static String bondStateStr(int state) {
        switch (state) {
            case BluetoothDevice.BOND_NONE:    return "NONE";
            case BluetoothDevice.BOND_BONDING: return "BONDING";
            case BluetoothDevice.BOND_BONDED:  return "BONDED";
            default: return "UNKNOWN(" + state + ")";
        }
    }

    // ─── Radio control panel ─────────────────────────────────────────────────

    // Hidden Soft AP states (from WifiManager source). Public SDK only exposes
    // them as ints; this app is built as a privileged system app so the
    // hidden setWifiApEnabled / getWifiApState reflection paths are available.
    private static final int WIFI_AP_STATE_DISABLING = 10;
    private static final int WIFI_AP_STATE_DISABLED  = 11;
    private static final int WIFI_AP_STATE_ENABLING  = 12;
    private static final int WIFI_AP_STATE_ENABLED   = 13;
    private static final int WIFI_AP_STATE_FAILED    = 14;

    private final BroadcastReceiver wifiApReceiver_ = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            String action = intent.getAction();
            if ("android.net.wifi.WIFI_AP_STATE_CHANGED".equals(action)) {
                int state = intent.getIntExtra("wifi_state", WIFI_AP_STATE_FAILED);
                Log.i(TAG, "Soft AP state changed: " + wifiApStateStr(state));
                updateSoftApButton();
            }
        }
    };

    private static String wifiApStateStr(int state) {
        switch (state) {
            case WIFI_AP_STATE_DISABLING: return "DISABLING";
            case WIFI_AP_STATE_DISABLED:  return "DISABLED";
            case WIFI_AP_STATE_ENABLING:  return "ENABLING";
            case WIFI_AP_STATE_ENABLED:   return "ENABLED";
            case WIFI_AP_STATE_FAILED:    return "FAILED";
            default: return "UNKNOWN(" + state + ")";
        }
    }

    private void toggleBluetooth() {
        BluetoothAdapter bt = BluetoothAdapter.getDefaultAdapter();
        if (bt == null) {
            Toast.makeText(this, "No Bluetooth adapter", Toast.LENGTH_SHORT).show();
            return;
        }
        if (bt.isEnabled()) {
            Log.i(TAG, "Disabling Bluetooth");
            bt.disable();
        } else {
            Log.i(TAG, "Enabling Bluetooth");
            bt.enable();
        }
    }

    private void updateBluetoothButton() {
        if (btBluetoothToggle_ == null) return;
        BluetoothAdapter bt = BluetoothAdapter.getDefaultAdapter();
        if (bt == null) {
            btBluetoothToggle_.setText("Bluetooth: N/A");
            btBluetoothToggle_.setEnabled(false);
            return;
        }
        int state = bt.getState();
        btBluetoothToggle_.setText("Bluetooth: " + btStateStr(state));
        // Disable while a transition is in flight to prevent double-click
        boolean transitioning = state == BluetoothAdapter.STATE_TURNING_ON
                              || state == BluetoothAdapter.STATE_TURNING_OFF;
        btBluetoothToggle_.setEnabled(!transitioning);
    }

    private int getSoftApState() {
        WifiManager wm = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        if (wm == null) return WIFI_AP_STATE_FAILED;
        try {
            Method m = wm.getClass().getMethod("getWifiApState");
            Object result = m.invoke(wm);
            return result instanceof Integer ? (Integer) result : WIFI_AP_STATE_FAILED;
        } catch (Throwable t) {
            Log.w(TAG, "getWifiApState reflection failed: " + t);
            return WIFI_AP_STATE_FAILED;
        }
    }

    private void toggleSoftAp() {
        WifiManager wm = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        if (wm == null) {
            Toast.makeText(this, "No Wifi service", Toast.LENGTH_SHORT).show();
            return;
        }
        int state = getSoftApState();
        boolean wantEnabled = (state != WIFI_AP_STATE_ENABLED && state != WIFI_AP_STATE_ENABLING);
        Log.i(TAG, "Toggling Soft AP -> " + (wantEnabled ? "ENABLE" : "DISABLE"));

        // Android 8+ removed setWifiApEnabled. The replacement is the
        // hidden @SystemApi pair startSoftAp(WifiConfiguration) /
        // stopSoftAp() which is reachable via reflection from a privileged
        // system app. NETWORK_STACK permission is required.
        try {
            if (wantEnabled) {
                Method m = wm.getClass().getMethod("startSoftAp", WifiConfiguration.class);
                Object ok = m.invoke(wm, (WifiConfiguration) null);
                if (ok instanceof Boolean && !((Boolean) ok)) {
                    Toast.makeText(this, "startSoftAp returned false", Toast.LENGTH_SHORT).show();
                }
            } else {
                Method m = wm.getClass().getMethod("stopSoftAp");
                Object ok = m.invoke(wm);
                if (ok instanceof Boolean && !((Boolean) ok)) {
                    Toast.makeText(this, "stopSoftAp returned false", Toast.LENGTH_SHORT).show();
                }
            }
        } catch (Throwable t) {
            Log.w(TAG, "Soft AP reflection failed: " + t);
            Toast.makeText(this, "Soft AP toggle failed: " + t.getMessage(),
                           Toast.LENGTH_LONG).show();
        }
    }

    private void updateSoftApButton() {
        if (btSoftApToggle_ == null) return;
        int state = getSoftApState();
        btSoftApToggle_.setText("Soft AP: " + wifiApStateStr(state));
        boolean transitioning = state == WIFI_AP_STATE_ENABLING || state == WIFI_AP_STATE_DISABLING;
        btSoftApToggle_.setEnabled(!transitioning);
    }

    // ─── Activity lifecycle ───────────────────────────────────────────────────

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_device_list);

        deviceList_ = findViewById(R.id.device_list);
        adapter_    = new DeviceAdapter(this, new ArrayList<ListItem>());
        deviceList_.setAdapter(adapter_);

        deviceList_.setOnItemClickListener((parent, view, position, id) -> {
            ListItem item = adapter_.getItem(position);
            if (item != null) onItemTapped(item);
        });

        deviceList_.setOnItemLongClickListener((parent, view, position, id) -> {
            ListItem item = adapter_.getItem(position);
            if (item != null) onItemLongPressed(item);
            return true;
        });

        // Radio control panel
        btBluetoothToggle_ = findViewById(R.id.btn_bluetooth_toggle);
        btSoftApToggle_    = findViewById(R.id.btn_softap_toggle);
        btBluetoothToggle_.setOnClickListener(v -> toggleBluetooth());
        btSoftApToggle_.setOnClickListener(v -> toggleSoftAp());

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
            new IntentFilter(AaSessionService.ACTION_SESSION_LIST_CHANGED));

        IntentFilter btFilter = new IntentFilter();
        btFilter.addAction(BluetoothAdapter.ACTION_STATE_CHANGED);
        btFilter.addAction(BluetoothDevice.ACTION_BOND_STATE_CHANGED);
        btFilter.addAction(BluetoothDevice.ACTION_ACL_CONNECTED);
        btFilter.addAction(BluetoothDevice.ACTION_ACL_DISCONNECTED);
        btFilter.addAction(BluetoothDevice.ACTION_UUID);
        registerReceiver(btEventReceiver_, btFilter);

        // Watch Soft AP state so the toggle button label stays in sync.
        IntentFilter wifiFilter = new IntentFilter();
        wifiFilter.addAction("android.net.wifi.WIFI_AP_STATE_CHANGED");
        registerReceiver(wifiApReceiver_, wifiFilter);

        updateBluetoothButton();
        updateSoftApButton();
    }

    @Override
    protected void onStop() {
        super.onStop();
        unregisterReceiver(listChangedReceiver_);
        unregisterReceiver(btEventReceiver_);
        unregisterReceiver(wifiApReceiver_);
        if (bound_) {
            unbindService(serviceConnection_);
            bound_   = false;
            service_ = null;
        }
    }

    // ─── Device list ─────────────────────────────────────────────────────────

    private void refreshDeviceList() {
        List<ListItem> items = new ArrayList<>();

        // 1. Live sessions from AaSessionService.
        List<SessionEntry> sessions = (bound_ && service_ != null)
            ? service_.getSessionList() : new ArrayList<>();
        for (SessionEntry e : sessions) {
            items.add(ListItem.forSession(e));
        }

        // 2. Paired BT devices that are not yet in a session (offer dial).
        BluetoothAdapter bt = BluetoothAdapter.getDefaultAdapter();
        if (bt != null && bt.isEnabled()) {
            for (BluetoothDevice device : bt.getBondedDevices()) {
                String addr = device.getAddress();
                String name = device.getName() != null ? device.getName() : addr;
                boolean alreadyListed = false;
                for (SessionEntry e : sessions) {
                    if (addr.equals(e.transportId)) { alreadyListed = true; break; }
                }
                if (!alreadyListed) {
                    items.add(ListItem.forIdleBt(name, addr));
                }
            }
        }

        adapter_.clear();
        adapter_.addAll(items);
        adapter_.notifyDataSetChanged();
    }

    // ─── Interactions ────────────────────────────────────────────────────────

    private void onItemTapped(ListItem item) {
        Log.i(TAG, "Item tapped: " + item.label() + " state=" + item.state);

        if (item.state == null) {
            // Idle BT entry — tapping does not dial yet (Phase 4 feature).
            // For now: ignore. WirelessMonitorService dials autonomously.
            Log.i(TAG, "Idle BT entry tap: dial flow handled by WirelessMonitorService");
            return;
        }

        switch (item.state) {
            case CONNECTING:
            case HANDSHAKING:
                Log.i(TAG, "Session not ready: " + item.state);
                return;
            case READY:
            case RUNNING:
            case BACKGROUND_AUDIO:
                if (bound_ && service_ != null) {
                    service_.activate(item.handle);
                }
                Intent intent = new Intent(this, AaDisplayActivity.class);
                intent.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
                startActivity(intent);
                break;
        }
    }

    private void onItemLongPressed(ListItem item) {
        if (!bound_ || service_ == null) return;
        if (item.state == null) return;
        Log.i(TAG, "Long press: disconnecting " + item.label());
        if (item.handle != 0) {
            service_.disconnectSession(item.handle);
        } else {
            service_.disconnectPending(item.transportId);
        }
    }

    // ─── ListItem model ──────────────────────────────────────────────────────

    static class ListItem {
        final long         handle;        // 0 if not yet a session
        final String       transportId;
        final String       displayName;
        final boolean      isWireless;
        final SessionState state;         // null = idle BT entry

        private ListItem(long handle, String transportId, String displayName,
                         boolean isWireless, SessionState state) {
            this.handle      = handle;
            this.transportId = transportId;
            this.displayName = displayName;
            this.isWireless  = isWireless;
            this.state       = state;
        }

        static ListItem forSession(SessionEntry e) {
            return new ListItem(e.handle, e.transportId, e.displayName, e.isWireless, e.state);
        }

        static ListItem forIdleBt(String name, String addr) {
            return new ListItem(0, addr, name, true, null);
        }

        String label() {
            return displayName + " [" + transportId + "]";
        }
    }

    // ─── List adapter ────────────────────────────────────────────────────────

    private static class DeviceAdapter extends ArrayAdapter<ListItem> {

        DeviceAdapter(Context context, List<ListItem> items) {
            super(context, R.layout.item_device, items);
        }

        @Override
        public View getView(int position, View convertView, ViewGroup parent) {
            if (convertView == null) {
                convertView = LayoutInflater.from(getContext())
                    .inflate(R.layout.item_device, parent, false);
            }
            ListItem item = getItem(position);
            if (item == null) return convertView;

            TextView nameView   = convertView.findViewById(R.id.device_name);
            TextView infoView   = convertView.findViewById(R.id.device_info);
            TextView statusView = convertView.findViewById(R.id.device_status);

            nameView.setText(item.displayName);
            infoView.setText(item.isWireless ? "Wireless" : "USB");

            String transport = item.isWireless ? "WiFi" : "USB";
            if (item.state == SessionState.RUNNING) {
                statusView.setVisibility(View.VISIBLE);
                statusView.setText(transport + " : Running");
                statusView.setTextColor(0xFF00ccff);
                convertView.setBackgroundColor(0xFF0d1f2d);
            } else if (item.state == SessionState.BACKGROUND_AUDIO) {
                statusView.setVisibility(View.VISIBLE);
                statusView.setText(transport + " : Audio only");
                statusView.setTextColor(0xFF00cccc);
                convertView.setBackgroundColor(0xFF0d1f1f);
            } else if (item.state == SessionState.READY) {
                statusView.setVisibility(View.VISIBLE);
                statusView.setText(transport + " : Ready");
                statusView.setTextColor(0xFF00cc66);
                convertView.setBackgroundColor(0xFF0d1f0d);
            } else if (item.state == SessionState.HANDSHAKING) {
                statusView.setVisibility(View.VISIBLE);
                statusView.setText(transport + " : Handshaking...");
                statusView.setTextColor(0xFFffaa00);
                convertView.setBackgroundColor(0xFF1f1a0d);
            } else if (item.state == SessionState.CONNECTING) {
                statusView.setVisibility(View.VISIBLE);
                statusView.setText(transport + " : Connecting...");
                statusView.setTextColor(0xFFffaa00);
                convertView.setBackgroundColor(0xFF1f1a0d);
            } else {
                statusView.setVisibility(View.GONE);
                convertView.setBackgroundColor(0x00000000);
            }

            return convertView;
        }
    }
}
