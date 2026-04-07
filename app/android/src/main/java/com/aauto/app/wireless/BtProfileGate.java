package com.aauto.app.wireless;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothProfile;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.lang.reflect.Method;
import java.util.HashSet;
import java.util.Set;

/**
 * Per-device gate that disables BT media profiles for the duration of a
 * wireless Android Auto session.
 *
 * Why:
 *   When a phone is wirelessly connected via AAW, audio flows over the AAP
 *   audio channel (WiFi/TCP). The BT pairing remains alive for RFCOMM (AAW
 *   control channel), but the system also keeps A2DP_SINK / AVRCP_CONTROLLER
 *   active for the same device. AOSP BluetoothMediaBrowserService reacts to
 *   the phone's AVRCP "playing" notifications by sending PASS THROUGH PAUSE
 *   back — interpreting "remote is playing" + "local audio focus held by
 *   another app" as a pause request. Phone obeys and AAP music stops 3-4
 *   seconds after starting.
 *
 *   Fix: while the wireless AA session is alive, force the phone's A2DP_SINK
 *   to be disconnected (and prevent reconnect). When A2DP_SINK is gone the
 *   BluetoothMediaBrowserService stops tracking the device's AVRCP state and
 *   no PAUSE is sent. AVRCP_CONTROLLER itself has no per-device priority API
 *   in Android 10, so we cannot disable it directly — instead we rely on the
 *   A2DP_SINK takedown to remove the trigger.
 *
 *   On session end, restore A2DP_SINK priority so normal BT media usage works
 *   again outside the AA session.
 *
 * Reactive disconnect:
 *   onWirelessDeviceReady runs while phone↔HU profile connections are still
 *   being negotiated, so an immediate disconnect call typically returns false
 *   (nothing to disconnect yet). We register a BroadcastReceiver for
 *   `android.bluetooth.a2dp-sink.profile.action.CONNECTION_STATE_CHANGED`
 *   and disconnect again whenever a blocked device transitions to CONNECTED.
 */
public class BtProfileGate {

    private static final String TAG = "AA.APP.BtProfileGate";

    // Hidden BluetoothProfile constants (Android 10).
    private static final int PROFILE_A2DP_SINK        = 11;
    private static final int PROFILE_AVRCP_CONTROLLER = 12;
    private static final int PRIORITY_OFF             = 0;
    private static final int PRIORITY_ON              = 100;

    // Hidden action / extra strings used by BluetoothA2dpSink.
    private static final String ACTION_A2DP_SINK_CONNECTION_STATE_CHANGED =
        "android.bluetooth.a2dp-sink.profile.action.CONNECTION_STATE_CHANGED";

    private final Context          context_;
    private final BluetoothAdapter adapter_;
    private final Handler          mainHandler_ = new Handler(Looper.getMainLooper());

    private BluetoothProfile a2dpSink_        = null;
    private BluetoothProfile avrcpController_ = null;
    private boolean          receiverRegistered_ = false;

    /** Devices currently blocked. Used to filter the broadcast and to restore on close(). */
    private final Set<String> blocked_ = new HashSet<>();

    public BtProfileGate(Context context) {
        context_ = context.getApplicationContext();
        adapter_ = BluetoothAdapter.getDefaultAdapter();
        if (adapter_ == null) {
            Log.w(TAG, "BluetoothAdapter unavailable");
            return;
        }
        adapter_.getProfileProxy(context_, listenerA2dp_,  PROFILE_A2DP_SINK);
        adapter_.getProfileProxy(context_, listenerAvrcp_, PROFILE_AVRCP_CONTROLLER);

        // Listen for A2DP_SINK connection state changes so we can disconnect
        // a blocked device the moment it actually connects.
        IntentFilter filter = new IntentFilter(ACTION_A2DP_SINK_CONNECTION_STATE_CHANGED);
        try {
            context_.registerReceiver(connectionStateReceiver_, filter);
            receiverRegistered_ = true;
        } catch (Exception e) {
            Log.w(TAG, "registerReceiver failed: " + e);
        }
    }

    /** Block media profiles for the given device. Idempotent. */
    public synchronized void block(String btAddress) {
        if (adapter_ == null || btAddress == null) return;
        if (!blocked_.add(btAddress)) return;

        BluetoothDevice device = safeGetDevice(btAddress);
        if (device == null) {
            blocked_.remove(btAddress);
            return;
        }

        Log.i(TAG, "Blocking media profiles for " + btAddress);
        applyBlock(device);

        // Even if disconnect returned false here (profile not yet connected),
        // the broadcast receiver will catch the upcoming CONNECTED transition
        // and disconnect again.
    }

    /** Restore media profiles for the given device. Idempotent. */
    public synchronized void restore(String btAddress) {
        if (adapter_ == null || btAddress == null) return;
        if (!blocked_.remove(btAddress)) return;

        BluetoothDevice device = safeGetDevice(btAddress);
        if (device == null) return;

        Log.i(TAG, "Restoring media profiles for " + btAddress);
        setPriority(a2dpSink_, device, PRIORITY_ON, "A2DP_SINK");
    }

    /** Restore all currently blocked devices and release proxies. */
    public synchronized void close() {
        for (String addr : new HashSet<>(blocked_)) {
            BluetoothDevice device = safeGetDevice(addr);
            if (device != null) setPriority(a2dpSink_, device, PRIORITY_ON, "A2DP_SINK");
        }
        blocked_.clear();

        if (receiverRegistered_) {
            try { context_.unregisterReceiver(connectionStateReceiver_); }
            catch (Exception ignored) {}
            receiverRegistered_ = false;
        }

        if (adapter_ != null) {
            if (a2dpSink_        != null) adapter_.closeProfileProxy(PROFILE_A2DP_SINK,        a2dpSink_);
            if (avrcpController_ != null) adapter_.closeProfileProxy(PROFILE_AVRCP_CONTROLLER, avrcpController_);
        }
        a2dpSink_        = null;
        avrcpController_ = null;
    }

    // ─── Internals ───────────────────────────────────────────────────────────

    private void applyBlock(BluetoothDevice device) {
        // A2DP_SINK: priority OFF blocks future auto-reconnects; disconnect
        // tears down any in-flight or established connection.
        setPriority(a2dpSink_, device, PRIORITY_OFF, "A2DP_SINK");
        disconnect(a2dpSink_, device, "A2DP_SINK");

        // AVRCP_CONTROLLER on Android 10 has no per-device priority API. We
        // still attempt disconnect via reflection in case the BSP exposed it.
        // Once A2DP_SINK is gone, BluetoothMediaBrowserService stops sending
        // PASS THROUGH PAUSE for this device.
        disconnect(avrcpController_, device, "AVRCP_CONTROLLER");
    }

    private BluetoothDevice safeGetDevice(String addr) {
        try {
            return adapter_.getRemoteDevice(addr);
        } catch (IllegalArgumentException e) {
            Log.w(TAG, "invalid BT address: " + addr);
            return null;
        }
    }

    // ─── BroadcastReceiver: react to A2DP_SINK CONNECTED ─────────────────────

    private final BroadcastReceiver connectionStateReceiver_ = new BroadcastReceiver() {
        @Override
        public void onReceive(Context ctx, Intent intent) {
            int state = intent.getIntExtra(BluetoothProfile.EXTRA_STATE, -1);
            BluetoothDevice device = intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE);
            if (device == null) return;
            String addr = device.getAddress();
            Log.i(TAG, "A2DP_SINK state change: " + addr + " state=" + state);

            synchronized (BtProfileGate.this) {
                if (!blocked_.contains(addr)) return;
            }

            if (state == BluetoothProfile.STATE_CONNECTED ||
                state == BluetoothProfile.STATE_CONNECTING) {
                // Re-apply block immediately. Use main handler to serialize
                // with block()/restore() and avoid touching state from the
                // broadcast thread directly.
                mainHandler_.post(() -> {
                    synchronized (BtProfileGate.this) {
                        if (!blocked_.contains(addr)) return;
                        Log.i(TAG, "Re-disconnecting blocked device on connect: " + addr);
                        applyBlock(device);
                    }
                });
            }
        }
    };

    // ─── Profile proxy listeners ─────────────────────────────────────────────

    private final BluetoothProfile.ServiceListener listenerA2dp_ =
        new BluetoothProfile.ServiceListener() {
            @Override
            public void onServiceConnected(int profile, BluetoothProfile proxy) {
                Log.i(TAG, "A2DP_SINK proxy connected");
                synchronized (BtProfileGate.this) {
                    a2dpSink_ = proxy;
                    for (String addr : blocked_) {
                        BluetoothDevice d = safeGetDevice(addr);
                        if (d != null) applyBlock(d);
                    }
                }
            }
            @Override
            public void onServiceDisconnected(int profile) {
                Log.i(TAG, "A2DP_SINK proxy disconnected");
                synchronized (BtProfileGate.this) { a2dpSink_ = null; }
            }
        };

    private final BluetoothProfile.ServiceListener listenerAvrcp_ =
        new BluetoothProfile.ServiceListener() {
            @Override
            public void onServiceConnected(int profile, BluetoothProfile proxy) {
                Log.i(TAG, "AVRCP_CONTROLLER proxy connected");
                synchronized (BtProfileGate.this) {
                    avrcpController_ = proxy;
                    for (String addr : blocked_) {
                        BluetoothDevice d = safeGetDevice(addr);
                        if (d != null) disconnect(proxy, d, "AVRCP_CONTROLLER");
                    }
                }
            }
            @Override
            public void onServiceDisconnected(int profile) {
                Log.i(TAG, "AVRCP_CONTROLLER proxy disconnected");
                synchronized (BtProfileGate.this) { avrcpController_ = null; }
            }
        };

    // ─── Reflection helpers ──────────────────────────────────────────────────

    private static void setPriority(BluetoothProfile proxy, BluetoothDevice device,
                                     int priority, String name) {
        if (proxy == null) return;
        try {
            Method m = proxy.getClass().getMethod("setPriority", BluetoothDevice.class, int.class);
            Object result = m.invoke(proxy, device, priority);
            Log.i(TAG, name + ".setPriority(" + device.getAddress() + "," + priority + ") -> " + result);
            return;
        } catch (NoSuchMethodException e) {
            // Try newer name.
        } catch (Exception e) {
            Log.e(TAG, name + ".setPriority failed: " + e);
            return;
        }
        try {
            Method m = proxy.getClass().getMethod("setConnectionPolicy",
                BluetoothDevice.class, int.class);
            Object result = m.invoke(proxy, device, priority);
            Log.i(TAG, name + ".setConnectionPolicy(" + device.getAddress() + "," + priority + ") -> " + result);
        } catch (NoSuchMethodException e) {
            // Profile doesn't expose a per-device priority API on this platform.
        } catch (Exception e) {
            Log.e(TAG, name + ".setConnectionPolicy failed: " + e);
        }
    }

    private static void disconnect(BluetoothProfile proxy, BluetoothDevice device, String name) {
        if (proxy == null) return;
        try {
            Method m = proxy.getClass().getMethod("disconnect", BluetoothDevice.class);
            Object result = m.invoke(proxy, device);
            Log.i(TAG, name + ".disconnect(" + device.getAddress() + ") -> " + result);
        } catch (NoSuchMethodException e) {
            // disconnect not exposed on this profile in this version — ignore.
        } catch (Exception e) {
            Log.w(TAG, name + ".disconnect failed: " + e);
        }
    }
}
