package com.aauto.app.core;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.util.Log;
import android.view.Surface;

import android.bluetooth.BluetoothAdapter;

import com.aauto.app.BuildInfo;
import com.aauto.app.MainActivity;

import android.system.Os;

import java.io.FileDescriptor;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Foreground service that owns the AA engine and all session state.
 *
 * Responsibilities:
 *   - Owns the JNI engine (one AAutoEngine, N sessions keyed by native handle).
 *   - Receives transport-ready events from UsbMonitorService / WirelessMonitorService
 *     and creates a native session per transport.
 *   - Maintains the activation policy: at most one session is active (sinks attached).
 *   - Routes the system Surface to the active session's video sink.
 *   - Broadcasts ACTION_SESSION_LIST_CHANGED whenever the list or any state changes.
 *   - Broadcasts ACTION_SESSION_ENDED(handle) when a session terminates.
 *
 * Activation model (Phase 3):
 *   activate(handle)   → audio detach old, video detach old, swap, then attach new
 *                        (only if a Surface is currently bound)
 *   deactivateAll()    → audio detach, video detach, no replacement
 *   onSurfaceReady     → record Surface, attach to active if any
 *   onSurfaceDestroyed → release Surface, detach from active if any
 *
 * Session lifecycle states:
 *   CONNECTING  - wireless RFCOMM handshake in progress (no native handle yet)
 *   HANDSHAKING - transport ready, native session created, AAP handshaking
 *   READY       - PhoneInfo received, listed in MainActivity, can be activated
 *   RUNNING     - currently active, sinks attached (at most one)
 */
public class AaSessionService extends Service {

    private static final String TAG               = "AA.APP.AaSessionService";
    private static final String NOTIFICATION_CHANNEL = "aa_session_service";
    private static final int    NOTIFICATION_ID   = 3;

    // Native AudioService channel indices (must match ServiceFactory ordering).
    private static final int AUDIO_CH_MEDIA    = 0;
    private static final int AUDIO_CH_GUIDANCE = 1;
    private static final int AUDIO_CH_SYSTEM   = 2;

    // ─── Intent actions for onStartCommand ───────────────────────────────────

    public static final String ACTION_USB_DEVICE_READY          = "com.aauto.app.ACTION_USB_DEVICE_READY";
    public static final String ACTION_USB_DEVICE_DETACHED       = "com.aauto.app.ACTION_USB_DEVICE_DETACHED";
    public static final String ACTION_WIRELESS_CONNECTING       = "com.aauto.app.ACTION_WIRELESS_CONNECTING";
    public static final String ACTION_WIRELESS_DEVICE_READY     = "com.aauto.app.ACTION_WIRELESS_DEVICE_READY";
    public static final String ACTION_WIRELESS_DEVICE_DETACHED  = "com.aauto.app.ACTION_WIRELESS_DEVICE_DETACHED";

    // ─── Intent extras ───────────────────────────────────────────────────────

    public static final String EXTRA_USB_FD        = "usb_fd";
    public static final String EXTRA_USB_EP_IN     = "usb_ep_in";
    public static final String EXTRA_USB_EP_OUT    = "usb_ep_out";
    public static final String EXTRA_DEVICE_ID     = "device_id";
    public static final String EXTRA_DEVICE_NAME   = "device_name";

    // ─── Broadcast actions ───────────────────────────────────────────────────

    /** Sent whenever the session list or any session state changes. */
    public static final String ACTION_SESSION_LIST_CHANGED = "com.aauto.app.ACTION_SESSION_LIST_CHANGED";

    /** Sent when a session terminates (transport disconnected or session closed). */
    public static final String ACTION_SESSION_ENDED        = "com.aauto.app.ACTION_SESSION_ENDED";

    /** Extra: native handle of the ended session. */
    public static final String EXTRA_SESSION_ENDED_HANDLE  = "session_ended_handle";

    // ─── Session model ───────────────────────────────────────────────────────

    public enum SessionState { CONNECTING, HANDSHAKING, READY, RUNNING }

    public static class SessionEntry {
        /** Native session handle, or 0 if still pending (wireless CONNECTING). */
        public       long         handle;
        /** Transport-level identifier (BT MAC, USB device id) — stable across the entry's life. */
        public final String       transportId;
        /** Human-readable name. Initialized from the monitor service, may be refined by PhoneInfo. */
        public       String       displayName;
        public final boolean      isWireless;
        public       SessionState state;
        public       String       phoneInstanceId;
        public       String       phoneLifetimeId;

        SessionEntry(long handle, String transportId, String displayName,
                     boolean isWireless, SessionState state) {
            this.handle      = handle;
            this.transportId = transportId;
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
    private final Handler     mainHandler_ = new Handler(Looper.getMainLooper());

    // ─── Session state ────────────────────────────────────────────────────────

    /** Pending wireless entries (CONNECTING, no handle yet), keyed by transportId. */
    private final Map<String, SessionEntry> pending_ = new LinkedHashMap<>();
    /** Active entries (handle assigned), keyed by native handle. */
    private final Map<Long, SessionEntry>   sessions_ = new LinkedHashMap<>();

    /** The currently active session (sinks attached), or 0 if none. */
    private long    activeHandle_   = 0;
    /** The system rendering Surface, or null if no AaDisplayActivity is showing. */
    private Surface currentSurface_ = null;
    private int     currentViewW_   = 0;
    private int     currentViewH_   = 0;

    // ─── Service lifecycle ────────────────────────────────────────────────────

    @Override
    public void onCreate() {
        super.onCreate();
        Log.i(TAG, "onCreate [build " + BuildInfo.BUILD_VERSION + "]");
        startForeground(NOTIFICATION_ID, buildNotification());
        String btAddress = getBluetoothAddress();
        nativeInit(btAddress);
        Log.i(TAG, "BT address: " + btAddress);
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

                long handle = nativeOnUsbDeviceReady(fd, epIn, epOut, id);
                if (handle == 0) {
                    Log.e(TAG, "nativeOnUsbDeviceReady failed for " + id);
                    break;
                }
                SessionEntry entry = new SessionEntry(handle, id, name, false, SessionState.HANDSHAKING);
                sessions_.put(handle, entry);
                broadcastSessionListChanged();
                bringMainActivityToFront();
                break;
            }
            case ACTION_USB_DEVICE_DETACHED: {
                String id = intentDeviceId(intent, "usb_device");
                Log.i(TAG, "USB device detached: " + id);
                stopAndRemoveByTransportId(id);
                break;
            }
            case ACTION_WIRELESS_CONNECTING: {
                String id   = intentDeviceId(intent, "wireless_device");
                String name = intent.getStringExtra(EXTRA_DEVICE_NAME);
                if (name == null) name = "Wireless Device";
                Log.i(TAG, "Wireless connecting: id=" + id);
                pending_.put(id, new SessionEntry(0, id, name, true, SessionState.CONNECTING));
                broadcastSessionListChanged();
                bringMainActivityToFront();
                break;
            }
            case ACTION_WIRELESS_DEVICE_READY:
                // Handled via direct binder call from WirelessMonitorService.
                Log.w(TAG, "ACTION_WIRELESS_DEVICE_READY via Intent — expected binder call");
                break;
            case ACTION_WIRELESS_DEVICE_DETACHED: {
                String id = intentDeviceId(intent, "wireless_device");
                Log.i(TAG, "Wireless device detached: " + id);
                stopAndRemoveByTransportId(id);
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

    // ─── Activation API (called by AaDisplayActivity / MainActivity) ─────────

    /**
     * Make the given session active. Detaches the previously active session
     * (audio first, then video — audio glitch prevention) and attaches the new
     * one if a rendering Surface is currently bound. If no Surface is bound,
     * the session is recorded as RUNNING and will attach when one arrives.
     */
    public synchronized void activate(long handle) {
        if (handle == 0 || handle == activeHandle_) {
            return;
        }
        SessionEntry next = sessions_.get(handle);
        if (next == null) {
            Log.w(TAG, "activate: unknown handle " + handle);
            return;
        }

        if (activeHandle_ != 0) {
            detachActive();
            SessionEntry prev = sessions_.get(activeHandle_);
            if (prev != null) prev.state = SessionState.READY;
        }

        activeHandle_ = handle;
        next.state    = SessionState.RUNNING;
        if (currentSurface_ != null) attachActive();
        Log.i(TAG, "activate: handle=" + handle + " surfaceBound=" + (currentSurface_ != null));
        broadcastSessionListChanged();
    }

    /** Detach all sinks from the active session, leaving it in READY state. */
    public synchronized void deactivateAll() {
        if (activeHandle_ == 0) return;
        Log.i(TAG, "deactivateAll: was handle=" + activeHandle_);
        detachActive();
        SessionEntry prev = sessions_.get(activeHandle_);
        if (prev != null) prev.state = SessionState.READY;
        activeHandle_ = 0;
        broadcastSessionListChanged();
    }

    /**
     * Called when the rendering Surface is available (or its dimensions change).
     * Attaches the Surface to whichever session is currently active.
     */
    public synchronized void onSurfaceReady(Surface surface, int width, int height) {
        Log.i(TAG, "onSurfaceReady: " + width + "x" + height);
        currentViewW_ = width;
        currentViewH_ = height;
        nativeSetViewSize(width, height);

        // SurfaceView fires surfaceChanged multiple times in quick succession
        // (e.g. once with the pre-immersive size, then again with the final
        // size) without recreating the underlying Surface. Rebuilding the
        // video sink each time tears down the running decoder and the new one
        // cannot decode P-frames until the next keyframe arrives — many
        // seconds away for AAP video. Skip the rebuild when the same Surface
        // re-fires; only the view size update above is needed.
        if (currentSurface_ == surface) {
            Log.i(TAG, "onSurfaceReady: same Surface, view size updated only");
            return;
        }
        currentSurface_ = surface;
        if (activeHandle_ != 0) attachActive();
    }

    /** Called when the rendering Surface is destroyed. Detaches sinks. */
    public synchronized void onSurfaceDestroyed() {
        Log.i(TAG, "onSurfaceDestroyed");
        if (activeHandle_ != 0) detachActive();
        currentSurface_ = null;
    }

    /** Delivers a touch event to the currently active session. */
    public void dispatchTouchEvent(int pointerId, float x, float y, int action) {
        long handle = activeHandle_;
        if (handle == 0) return;
        nativeDispatchTouchEvent(handle, pointerId, x, y, action);
    }

    /**
     * Called directly by WirelessMonitorService via LocalBinder when the RFCOMM
     * handshake completes. serverFd is a pre-bound, listening TCP socket; ownership
     * transfers to the native layer (dup'd there).
     */
    public void onWirelessDeviceReady(String deviceId, String deviceName, FileDescriptor serverFd) {
        Log.i(TAG, "onWirelessDeviceReady (binder): id=" + deviceId);
        String name = deviceName != null ? deviceName : "Wireless Device";

        long handle = nativeOnWirelessDeviceReady(deviceId, serverFd);
        try { Os.close(serverFd); } catch (Exception ignored) {}
        if (handle == 0) {
            Log.e(TAG, "nativeOnWirelessDeviceReady failed for " + deviceId);
            pending_.remove(deviceId);
            broadcastSessionListChanged();
            return;
        }

        // Promote the pending CONNECTING entry to a real session.
        SessionEntry entry = pending_.remove(deviceId);
        if (entry == null) {
            entry = new SessionEntry(handle, deviceId, name, true, SessionState.HANDSHAKING);
        } else {
            entry.handle = handle;
            entry.state  = SessionState.HANDSHAKING;
        }
        sessions_.put(handle, entry);
        broadcastSessionListChanged();
        bringMainActivityToFront();
    }

    /** Returns the handle of the currently active session, or 0 if none. */
    public synchronized long getActiveHandle() {
        return activeHandle_;
    }

    /** Returns a snapshot of all sessions (pending CONNECTING + handshaking/ready/running). */
    public synchronized List<SessionEntry> getSessionList() {
        List<SessionEntry> out = new ArrayList<>(pending_.size() + sessions_.size());
        out.addAll(pending_.values());
        out.addAll(sessions_.values());
        return out;
    }

    /** Disconnect a session by native handle. */
    public synchronized void disconnectSession(long handle) {
        Log.i(TAG, "disconnectSession: handle=" + handle);
        stopAndRemoveByHandle(handle);
    }

    /** Disconnect a pending wireless entry (CONNECTING) by transport id. */
    public synchronized void disconnectPending(String transportId) {
        Log.i(TAG, "disconnectPending: " + transportId);
        if (pending_.remove(transportId) != null) {
            broadcastSessionListChanged();
        }
    }

    // ─── Upcalls from native (called from session callback threads) ──────────

    /** Invoked by JNI when the phone identifies itself in ServiceDiscoveryRequest. */
    @SuppressWarnings("unused")  // called from JNI
    private void onPhoneInfoFromNative(long handle, String instanceId, String lifetimeId,
                                       String deviceName, String labelText) {
        mainHandler_.post(() -> {
            synchronized (this) {
                SessionEntry entry = sessions_.get(handle);
                if (entry == null) return;
                entry.phoneInstanceId = instanceId;
                entry.phoneLifetimeId = lifetimeId;
                if (deviceName != null && !deviceName.isEmpty()) {
                    entry.displayName = deviceName;
                }
                if (entry.state == SessionState.HANDSHAKING) {
                    entry.state = SessionState.READY;
                }
                Log.i(TAG, "PhoneInfo handle=" + handle
                        + " instance=" + instanceId + " lifetime=" + lifetimeId
                        + " name=" + deviceName + " label=" + labelText);
            }
            broadcastSessionListChanged();
        });
    }

    /** Invoked by JNI when a session terminates (transport disconnect or explicit Stop). */
    @SuppressWarnings("unused")  // called from JNI
    private void onSessionClosedFromNative(long handle) {
        mainHandler_.post(() -> {
            synchronized (this) {
                SessionEntry entry = sessions_.remove(handle);
                if (entry == null) return;
                if (activeHandle_ == handle) {
                    // Active session vanished — drop the active reference. The
                    // sinks were owned by the now-gone native session, so no
                    // explicit detach is needed. AaDisplayActivity will see
                    // ACTION_SESSION_ENDED and finish.
                    activeHandle_ = 0;
                }
                Log.i(TAG, "Session closed handle=" + handle);
            }
            broadcastSessionListChanged();
            broadcastSessionEnded(handle);
        });
    }

    // ─── Internal helpers ────────────────────────────────────────────────────

    private void stopAndRemoveByTransportId(String transportId) {
        // Pending wireless entries that never got a handle.
        if (pending_.remove(transportId) != null) {
            broadcastSessionListChanged();
            return;
        }
        // Look up by transportId in the active map.
        for (SessionEntry e : sessions_.values()) {
            if (transportId.equals(e.transportId)) {
                stopAndRemoveByHandle(e.handle);
                return;
            }
        }
    }

    private void stopAndRemoveByHandle(long handle) {
        SessionEntry entry = sessions_.remove(handle);
        if (entry == null) return;
        if (activeHandle_ == handle) {
            detachActive();
            activeHandle_ = 0;
        }
        nativeStopSession(handle);
        broadcastSessionListChanged();
        broadcastSessionEnded(handle);
    }

    /** Attach sinks to the active session. Caller must hold the monitor and ensure
     *  activeHandle_ != 0 and currentSurface_ != null. */
    private void attachActive() {
        if (activeHandle_ == 0 || currentSurface_ == null) return;
        nativeAttachVideo(activeHandle_, currentSurface_);
        nativeAttachAudio(activeHandle_, AUDIO_CH_MEDIA);
        nativeAttachAudio(activeHandle_, AUDIO_CH_GUIDANCE);
        nativeAttachAudio(activeHandle_, AUDIO_CH_SYSTEM);
    }

    /** Detach sinks from the active session. Audio first to avoid glitches. */
    private void detachActive() {
        if (activeHandle_ == 0) return;
        nativeDetachAudio(activeHandle_, AUDIO_CH_MEDIA);
        nativeDetachAudio(activeHandle_, AUDIO_CH_GUIDANCE);
        nativeDetachAudio(activeHandle_, AUDIO_CH_SYSTEM);
        nativeDetachVideo(activeHandle_);
    }

    /** Bring MainActivity forward when no session is currently displaying. */
    private void bringMainActivityToFront() {
        if (activeHandle_ != 0) return;
        Intent intent = new Intent(this, MainActivity.class);
        intent.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        startActivity(intent);
    }

    private void broadcastSessionListChanged() {
        Intent intent = new Intent(ACTION_SESSION_LIST_CHANGED);
        intent.setPackage(getPackageName());
        sendBroadcast(intent);
    }

    private void broadcastSessionEnded(long handle) {
        Intent intent = new Intent(ACTION_SESSION_ENDED);
        intent.putExtra(EXTRA_SESSION_ENDED_HANDLE, handle);
        intent.setPackage(getPackageName());
        sendBroadcast(intent);
    }

    private static String getBluetoothAddress() {
        BluetoothAdapter bt = BluetoothAdapter.getDefaultAdapter();
        if (bt != null) {
            String addr = bt.getAddress();
            if (addr != null && !addr.isEmpty()) return addr;
        }
        return "00:11:22:33:44:55";
    }

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

    private native void nativeInit(String btAddress);
    private native void nativeDestroy();

    /** Builds and starts a USB session; returns the native session handle (0 on failure). */
    private native long nativeOnUsbDeviceReady(int fd, int epIn, int epOut, String transportId);

    /** Builds and starts a wireless session; returns the native session handle (0 on failure). */
    private native long nativeOnWirelessDeviceReady(String transportId, FileDescriptor serverFd);

    /** Stops a session and frees its resources. Safe to call on an unknown handle. */
    private native void nativeStopSession(long handle);

    /** Attach the rendering surface to a session's video pipeline. */
    private native void nativeAttachVideo(long handle, Surface surface);
    /** Detach the rendering surface, releasing the decoder. */
    private native void nativeDetachVideo(long handle);

    /** Attach an audio output for one channel (0=MEDIA, 1=GUIDANCE, 2=SYSTEM). */
    private native void nativeAttachAudio(long handle, int channelIdx);
    /** Detach an audio output. */
    private native void nativeDetachAudio(long handle, int channelIdx);

    /** Update the SurfaceView dimensions used to scale touch coordinates. */
    private native void nativeSetViewSize(int width, int height);

    /** Dispatch a touch event to the given session's InputService. */
    private native void nativeDispatchTouchEvent(long handle, int pointerId, float x, float y, int action);
}
