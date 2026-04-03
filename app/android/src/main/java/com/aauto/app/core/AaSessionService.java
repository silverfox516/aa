package com.aauto.app.core;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;
import android.util.Log;
import android.view.Surface;

import com.aauto.app.BuildInfo;
import com.aauto.app.MainActivity;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Foreground service that owns the AA engine lifecycle.
 *
 * Responsibilities:
 *   - Owns the JNI engine (DeviceManager, AAutoEngine, AndroidPlatform).
 *   - Receives USB/wireless transport events via onStartCommand.
 *   - Tracks connected devices and their state (CONNECTED / RUNNING).
 *   - Provides a LocalBinder for AaDisplayActivity to deliver surface and touch events.
 *   - Broadcasts ACTION_DEVICE_LIST_CHANGED when the device list changes.
 *   - Broadcasts ACTION_SESSION_ENDED(deviceId) when a device disconnects.
 *
 * Session lifecycle:
 *   CONNECTED = transport connected, AAP channels open, VideoFocusNotification(NATIVE)
 *               — phone is connected but not streaming AV.
 *   RUNNING   = VideoFocusNotification(PROJECTED) sent
 *               — phone is streaming AV to the HU display.
 *
 * Focus transitions are driven by the surface lifecycle in AaDisplayActivity:
 *   surfaceCreated  → onSurfaceReady()     → nativeSurfaceReady()     → VideoFocusGain
 *   surfaceDestroyed → onSurfaceDestroyed() → nativeSurfaceDestroyed() → VideoFocusLoss
 */
public class AaSessionService extends Service {

    private static final String TAG               = "AA.AaSessionService";
    private static final String NOTIFICATION_CHANNEL = "aa_session_service";
    private static final int    NOTIFICATION_ID   = 3;

    // ─── Intent actions for onStartCommand ───────────────────────────────────

    public static final String ACTION_USB_DEVICE_READY       = "com.aauto.app.ACTION_USB_DEVICE_READY";
    public static final String ACTION_USB_DEVICE_DETACHED    = "com.aauto.app.ACTION_USB_DEVICE_DETACHED";
    public static final String ACTION_WIRELESS_DEVICE_READY  = "com.aauto.app.ACTION_WIRELESS_DEVICE_READY";
    public static final String ACTION_WIRELESS_DEVICE_DETACHED = "com.aauto.app.ACTION_WIRELESS_DEVICE_DETACHED";

    // ─── Intent extras ───────────────────────────────────────────────────────

    public static final String EXTRA_USB_FD       = "usb_fd";
    public static final String EXTRA_USB_EP_IN    = "usb_ep_in";
    public static final String EXTRA_USB_EP_OUT   = "usb_ep_out";
    public static final String EXTRA_DEVICE_ID    = "device_id";
    public static final String EXTRA_DEVICE_NAME  = "device_name";
    public static final String EXTRA_WIRELESS_IP  = "wireless_ip";
    public static final String EXTRA_WIRELESS_PORT = "wireless_port";

    // ─── Broadcast actions ───────────────────────────────────────────────────

    /** Sent whenever the device list or any device state changes. */
    public static final String ACTION_DEVICE_LIST_CHANGED = "com.aauto.app.ACTION_DEVICE_LIST_CHANGED";

    /** Sent when a device session ends (transport disconnected). */
    public static final String ACTION_SESSION_ENDED       = "com.aauto.app.ACTION_SESSION_ENDED";

    /** Extra: deviceId of the ended session. */
    public static final String EXTRA_SESSION_ENDED_ID     = "session_ended_id";

    // ─── Device model ────────────────────────────────────────────────────────

    public enum DeviceState { CONNECTED, RUNNING }

    public static class DeviceEntry {
        public final String      deviceId;
        public final String      displayName;
        public final boolean     isWireless;
        public       DeviceState state;

        DeviceEntry(String deviceId, String displayName, boolean isWireless, DeviceState state) {
            this.deviceId    = deviceId;
            this.displayName = displayName;
            this.isWireless  = isWireless;
            this.state       = state;
        }
    }

    static {
        System.loadLibrary("aauto_jni");
    }

    // ─── LocalBinder ─────────────────────────────────────────────────────────

    public class LocalBinder extends Binder {
        public AaSessionService getService() { return AaSessionService.this; }
    }

    private final LocalBinder binder_ = new LocalBinder();

    // ─── Device state ─────────────────────────────────────────────────────────

    /** Insertion-ordered map: deviceId → DeviceEntry. Accessed on main thread only. */
    private final Map<String, DeviceEntry> devices_ = new LinkedHashMap<>();

    // ─── Service lifecycle ────────────────────────────────────────────────────

    @Override
    public void onCreate() {
        super.onCreate();
        Log.i(TAG, "onCreate [build " + BuildInfo.BUILD_VERSION + "]");
        startForeground(NOTIFICATION_ID, buildNotification());
        nativeInit();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null) return START_STICKY;
        String action = intent.getAction();
        if (action == null) return START_STICKY;

        switch (action) {
            case ACTION_USB_DEVICE_READY: {
                int    fd    = intent.getIntExtra(EXTRA_USB_FD, -1);
                int    epIn  = intent.getIntExtra(EXTRA_USB_EP_IN, 0);
                int    epOut = intent.getIntExtra(EXTRA_USB_EP_OUT, 0);
                String id    = intentDeviceId(intent, "usb_device");
                String name  = intent.getStringExtra(EXTRA_DEVICE_NAME);
                if (name == null) name = "USB Device";
                Log.i(TAG, "USB device ready: id=" + id + " fd=" + fd);
                devices_.put(id, new DeviceEntry(id, name, false, DeviceState.CONNECTED));
                nativeOnUsbDeviceReady(fd, epIn, epOut, id);
                broadcastDeviceListChanged();
                bringMainActivityToFront();
                break;
            }
            case ACTION_USB_DEVICE_DETACHED: {
                String id = intentDeviceId(intent, "usb_device");
                Log.i(TAG, "USB device detached: " + id);
                devices_.remove(id);
                nativeOnUsbDeviceDetached(id);
                broadcastDeviceListChanged();
                broadcastSessionEnded(id);
                break;
            }
            case ACTION_WIRELESS_DEVICE_READY: {
                String ip   = intent.getStringExtra(EXTRA_WIRELESS_IP);
                int    port = intent.getIntExtra(EXTRA_WIRELESS_PORT, 0);
                String id   = intentDeviceId(intent, "wireless_device");
                String name = intent.getStringExtra(EXTRA_DEVICE_NAME);
                if (name == null) name = "Wireless Device";
                Log.i(TAG, "Wireless device ready: id=" + id + " ip=" + ip);
                devices_.put(id, new DeviceEntry(id, name, true, DeviceState.CONNECTED));
                nativeOnWirelessDeviceReady(ip != null ? ip : "", port, id);
                broadcastDeviceListChanged();
                bringMainActivityToFront();
                break;
            }
            case ACTION_WIRELESS_DEVICE_DETACHED: {
                String id = intentDeviceId(intent, "wireless_device");
                Log.i(TAG, "Wireless device detached: " + id);
                devices_.remove(id);
                nativeOnWirelessDeviceDetached(id);
                broadcastDeviceListChanged();
                broadcastSessionEnded(id);
                break;
            }
        }
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        Log.i(TAG, "onDestroy");
        nativeDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return binder_;
    }

    // ─── Binder API (called by AaDisplayActivity) ─────────────────────────────

    /**
     * Called when the rendering surface is available.
     * Sets the surface on the video decoder and sends VideoFocusGain to the phone.
     */
    public void onSurfaceReady(String deviceId, Surface surface, int width, int height) {
        Log.i(TAG, "onSurfaceReady: deviceId=" + deviceId + " " + width + "x" + height);
        nativeSetViewSize(width, height);
        nativeSurfaceReady(deviceId, surface);
        DeviceEntry entry = devices_.get(deviceId);
        if (entry != null) {
            entry.state = DeviceState.RUNNING;
            broadcastDeviceListChanged();
        }
    }

    /**
     * Called when the rendering surface is destroyed.
     * Sends VideoFocusLoss to the phone and clears the decoder surface.
     * The session remains alive (CONNECTED state).
     */
    public void onSurfaceDestroyed(String deviceId) {
        Log.i(TAG, "onSurfaceDestroyed: deviceId=" + deviceId);
        nativeSurfaceDestroyed(deviceId);
        DeviceEntry entry = devices_.get(deviceId);
        if (entry != null) {
            entry.state = DeviceState.CONNECTED;
            broadcastDeviceListChanged();
        }
    }

    /** Delivers a touch event to the active session's InputService. */
    public void dispatchTouchEvent(int pointerId, float x, float y, int action) {
        nativeDispatchTouchEvent(pointerId, x, y, action);
    }

    /** Returns a snapshot of the current device list. */
    public List<DeviceEntry> getDeviceList() {
        return new ArrayList<>(devices_.values());
    }

    /**
     * Disconnects the given device: stops its session, removes from the list,
     * and broadcasts ACTION_SESSION_ENDED so AaDisplayActivity can finish.
     */
    public void disconnectDevice(String deviceId) {
        Log.i(TAG, "disconnectDevice: " + deviceId);
        DeviceEntry entry = devices_.remove(deviceId);
        if (entry == null) return;
        if (entry.isWireless) {
            nativeOnWirelessDeviceDetached(deviceId);
        } else {
            nativeOnUsbDeviceDetached(deviceId);
        }
        broadcastDeviceListChanged();
        broadcastSessionEnded(deviceId);
    }

    // ─── Broadcast helpers ────────────────────────────────────────────────────

    /**
     * Brings MainActivity to the foreground if no session is currently RUNNING.
     * Called when a new device connects so the user sees the device list immediately.
     * If AaDisplayActivity is active (RUNNING state), it is left undisturbed.
     */
    private void bringMainActivityToFront() {
        if (hasRunningSession()) return;
        Intent intent = new Intent(this, MainActivity.class);
        intent.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        startActivity(intent);
    }

    private boolean hasRunningSession() {
        for (DeviceEntry entry : devices_.values()) {
            if (entry.state == DeviceState.RUNNING) return true;
        }
        return false;
    }

    private void broadcastDeviceListChanged() {
        Intent intent = new Intent(ACTION_DEVICE_LIST_CHANGED);
        intent.setPackage(getPackageName());
        sendBroadcast(intent);
    }

    private void broadcastSessionEnded(String deviceId) {
        Intent intent = new Intent(ACTION_SESSION_ENDED);
        intent.putExtra(EXTRA_SESSION_ENDED_ID, deviceId);
        intent.setPackage(getPackageName());
        sendBroadcast(intent);
    }

    // ─── Private helpers ──────────────────────────────────────────────────────

    private static String intentDeviceId(Intent intent, String fallback) {
        String id = intent.getStringExtra(EXTRA_DEVICE_ID);
        return id != null ? id : fallback;
    }

    private Notification buildNotification() {
        NotificationChannel channel = new NotificationChannel(
                NOTIFICATION_CHANNEL, "Android Auto Session",
                NotificationManager.IMPORTANCE_MIN);
        channel.setShowBadge(false);
        getSystemService(NotificationManager.class).createNotificationChannel(channel);
        return new Notification.Builder(this, NOTIFICATION_CHANNEL)
                .setContentTitle("Android Auto")
                .setSmallIcon(android.R.drawable.ic_menu_info_details)
                .setOngoing(true)
                .setShowWhen(false)
                .build();
    }

    // ─── Native methods ───────────────────────────────────────────────────────

    private native void nativeInit();
    private native void nativeDestroy();
    private native void nativeSurfaceReady(String deviceId, Surface surface);
    private native void nativeSurfaceDestroyed(String deviceId);
    private native void nativeSetViewSize(int width, int height);
    private native void nativeOnUsbDeviceReady(int fd, int epIn, int epOut, String deviceId);
    private native void nativeOnUsbDeviceDetached(String deviceId);
    private native void nativeOnWirelessDeviceReady(String ip, int port, String deviceId);
    private native void nativeOnWirelessDeviceDetached(String deviceId);
    private native void nativeDispatchTouchEvent(int pointerId, float x, float y, int action);
}
