package com.aauto.app.wireless;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.bluetooth.BluetoothAdapter;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.ServiceConnection;
import android.net.wifi.WifiConfiguration;
import android.net.wifi.WifiManager;
import android.os.IBinder;
import android.system.Os;
import android.system.OsConstants;
import android.util.Log;

import com.aauto.app.BuildInfo;
import com.aauto.app.core.AaSessionService;

import java.io.FileDescriptor;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Foreground service that manages the Wireless Android Auto (AAW) lifecycle.
 *
 * Owns the BT RFCOMM listener (BluetoothWirelessManager) and the pre-bound
 * TCP server socket. Bridges lifecycle events between the RFCOMM control
 * channel and the AAP TCP data channel:
 *
 *   RFCOMM close  -> tears down the AAP session (TCP transport)
 *   AAP session close (TCP) -> tears down the RFCOMM keep-alive
 *   BT off / SoftAP off    -> tears down everything
 *
 * This guarantees the invariant: a wireless session is valid only while
 * BOTH BT and WiFi are alive.
 */
public class WirelessMonitorService extends Service
        implements BluetoothWirelessManager.Listener {

    private static final String TAG                  = "AA.APP.WirelessMonitorSvc";
    private static final String NOTIFICATION_CHANNEL = "aa_wireless_monitor";
    private static final int    NOTIFICATION_ID      = 2;
    private static final int    TCP_PORT             = 5277;

    public static final String ACTION_STOP_WIRELESS = "com.aauto.app.ACTION_STOP_WIRELESS";

    private BluetoothWirelessManager            wirelessManager_;
    private BluetoothWirelessManager.HotspotConfig hotspotConfig_;

    private FileDescriptor tcpServerFd_ = null;

    // ─── AaSessionService binding ─────────────────────────────────────────────

    private AaSessionService sessionService_  = null;
    private boolean          sessionBound_    = false;

    private final ServiceConnection sessionConn_ = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName name, IBinder binder) {
            sessionService_ = ((AaSessionService.LocalBinder) binder).getService();
            sessionBound_   = true;
            Log.i(TAG, "AaSessionService bound");
        }

        @Override
        public void onServiceDisconnected(ComponentName name) {
            sessionService_ = null;
            sessionBound_   = false;
        }
    };

    // ─── BT state receiver ───────────────────────────────────────────────────

    private final BroadcastReceiver btStateReceiver_ = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            int state = intent.getIntExtra(BluetoothAdapter.EXTRA_STATE, BluetoothAdapter.ERROR);
            if (state == BluetoothAdapter.STATE_ON) {
                Log.i(TAG, "Bluetooth on — starting RFCOMM listener");
                startListeningIfReady();
            } else if (state == BluetoothAdapter.STATE_TURNING_OFF) {
                Log.i(TAG, "Bluetooth turning off — stopping RFCOMM + tearing down wireless sessions");
                if (wirelessManager_ != null) wirelessManager_.stop();
                teardownAllWirelessSessions("BT off");
            }
        }
    };

    // ─── SoftAP state receiver ────────────────────────────────────────────────

    private final BroadcastReceiver apStateReceiver_ = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            int state = intent.getIntExtra(WifiManager.EXTRA_WIFI_AP_STATE,
                                           WifiManager.WIFI_AP_STATE_FAILED);
            if (state == WifiManager.WIFI_AP_STATE_ENABLED) {
                Log.i(TAG, "SoftAP enabled — reading hotspot config");
                hotspotConfig_ = readHotspotConfig();
                startListeningIfReady();
            } else if (state == WifiManager.WIFI_AP_STATE_DISABLED) {
                Log.i(TAG, "SoftAP disabled — stopping RFCOMM + tearing down wireless sessions");
                hotspotConfig_ = null;
                if (wirelessManager_ != null) wirelessManager_.stop();
                teardownAllWirelessSessions("SoftAP off");
            }
        }
    };

    // ─── AAP session end receiver (TCP close -> RFCOMM close) ────────────────

    private final BroadcastReceiver sessionEndReceiver_ = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            boolean isWireless = intent.getBooleanExtra("is_wireless", false);
            if (!isWireless) return;
            String deviceId = intent.getStringExtra(AaSessionService.EXTRA_DEVICE_ID);
            Log.i(TAG, "AAP session ended for wireless device " + deviceId
                       + " — closing RFCOMM keep-alive");
            if (wirelessManager_ != null) {
                wirelessManager_.closeCurrentClient();
            }
        }
    };

    // ─── Service lifecycle ────────────────────────────────────────────────────

    @Override
    public void onCreate() {
        super.onCreate();
        Log.i(TAG, "onCreate [build " + BuildInfo.BUILD_VERSION + "]");
        startForeground(NOTIFICATION_ID, buildNotification());

        Intent sessionIntent = new Intent(this, AaSessionService.class);
        startForegroundService(sessionIntent);
        bindService(sessionIntent, sessionConn_, Context.BIND_AUTO_CREATE);

        registerReceiver(btStateReceiver_,
            new IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED));
        registerReceiver(apStateReceiver_,
            new IntentFilter("android.net.wifi.WIFI_AP_STATE_CHANGED"));
        registerReceiver(sessionEndReceiver_,
            new IntentFilter(AaSessionService.ACTION_SESSION_ENDED));

        WifiManager wm = (WifiManager) getSystemService(Context.WIFI_SERVICE);
        if (wm.getWifiApState() == WifiManager.WIFI_AP_STATE_ENABLED) {
            hotspotConfig_ = readHotspotConfig();
        }

        startListeningIfReady();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_STOP_WIRELESS.equals(intent.getAction())) {
            Log.i(TAG, "Stop requested");
            if (wirelessManager_ != null) wirelessManager_.stop();
            stopSelf();
            return START_NOT_STICKY;
        }
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        Log.i(TAG, "onDestroy");
        unregisterReceiver(btStateReceiver_);
        unregisterReceiver(apStateReceiver_);
        unregisterReceiver(sessionEndReceiver_);
        if (wirelessManager_ != null) wirelessManager_.stop();
        closeTcpServerFd();
        if (sessionBound_) {
            unbindService(sessionConn_);
            sessionBound_   = false;
            sessionService_ = null;
        }
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }

    // ─── BluetoothWirelessManager.Listener ───────────────────────────────────

    @Override
    public void onDeviceConnecting(String deviceId, String deviceName) {
        Log.i(TAG, "RFCOMM connected from " + deviceId + " — handshake starting");
        Intent intent = new Intent(this, AaSessionService.class);
        intent.setAction(AaSessionService.ACTION_WIRELESS_CONNECTING);
        intent.putExtra(AaSessionService.EXTRA_DEVICE_ID, deviceId);
        intent.putExtra(AaSessionService.EXTRA_DEVICE_NAME, deviceName);
        startForegroundService(intent);
    }

    @Override
    public void onDeviceReady(String deviceId, String deviceName) {
        Log.i(TAG, "Handshake complete: " + deviceId + " — transferring TCP fd to session");

        FileDescriptor fd = tcpServerFd_;
        tcpServerFd_ = null;

        if (sessionService_ != null && fd != null) {
            sessionService_.onWirelessDeviceReady(deviceId, deviceName, fd);
        } else {
            Log.e(TAG, "Cannot transfer TCP fd: service=" + sessionService_ + " fd=" + fd);
            closeFd(fd);
        }

        rebindTcpServerFdOnly();
    }

    @Override
    public void onConnectionFailed(String deviceId, String reason) {
        Log.e(TAG, "Wireless connection failed for " + deviceId + ": " + reason);
    }

    @Override
    public void onDeviceDisconnected(String deviceId, String reason) {
        Log.i(TAG, "Wireless peer disconnected: " + deviceId + " (" + reason + ")");
        // RFCOMM ended -> tear down the AAP session (TCP transport) as well.
        Intent intent = new Intent(this, AaSessionService.class);
        intent.setAction(AaSessionService.ACTION_WIRELESS_DEVICE_DETACHED);
        intent.putExtra(AaSessionService.EXTRA_DEVICE_ID, deviceId);
        startForegroundService(intent);
    }

    // ─── Private helpers ──────────────────────────────────────────────────────

    private void startListeningIfReady() {
        BluetoothAdapter bt = BluetoothAdapter.getDefaultAdapter();
        if (bt == null || !bt.isEnabled()) {
            Log.i(TAG, "BT not ready, skipping listen");
            return;
        }
        if (hotspotConfig_ == null) {
            Log.w(TAG, "No hotspot config yet, skipping listen");
            return;
        }

        if (wirelessManager_ != null) wirelessManager_.stop();

        closeTcpServerFd();
        FileDescriptor fd = preBoundTcpSocket();
        if (fd == null) {
            Log.e(TAG, "TCP pre-bind failed, aborting wireless listen");
            return;
        }
        tcpServerFd_ = fd;

        wirelessManager_ = new BluetoothWirelessManager(this, this, hotspotConfig_);
        wirelessManager_.startListening();
    }

    /**
     * Drop every wireless session that AaSessionService still knows about.
     */
    private void teardownAllWirelessSessions(String reason) {
        if (sessionService_ == null) return;
        List<AaSessionService.SessionEntry> snapshot =
            new ArrayList<>(sessionService_.getSessionList());
        for (AaSessionService.SessionEntry e : snapshot) {
            if (!e.isWireless) continue;
            Log.i(TAG, "Tearing down wireless session " + e.transportId + " (" + reason + ")");
            if (e.handle != 0) {
                sessionService_.disconnectSession(e.handle);
            } else {
                sessionService_.disconnectPending(e.transportId);
            }
        }
    }

    private void rebindTcpServerFdOnly() {
        closeTcpServerFd();
        FileDescriptor fd = preBoundTcpSocket();
        if (fd == null) {
            Log.e(TAG, "TCP rebind failed — next wireless session will fail");
            return;
        }
        tcpServerFd_ = fd;
    }

    private FileDescriptor preBoundTcpSocket() {
        try {
            FileDescriptor fd = Os.socket(OsConstants.AF_INET, OsConstants.SOCK_STREAM, 0);
            Os.setsockoptInt(fd, OsConstants.SOL_SOCKET, OsConstants.SO_REUSEADDR, 1);
            Os.bind(fd, InetAddress.getByName("0.0.0.0"), TCP_PORT);
            Os.listen(fd, 1);
            Log.i(TAG, "TCP server pre-bound on port " + TCP_PORT);
            return fd;
        } catch (Exception e) {
            Log.e(TAG, "TCP pre-bind failed: " + e.getMessage());
            return null;
        }
    }

    private void closeTcpServerFd() {
        closeFd(tcpServerFd_);
        tcpServerFd_ = null;
    }

    private static void closeFd(FileDescriptor fd) {
        if (fd == null) return;
        try { Os.close(fd); } catch (Exception ignored) {}
    }

    @SuppressWarnings("deprecation")
    private BluetoothWirelessManager.HotspotConfig readHotspotConfig() {
        WifiManager wm = (WifiManager) getSystemService(Context.WIFI_SERVICE);
        WifiConfiguration apConfig = wm.getWifiApConfiguration();
        if (apConfig == null) {
            Log.e(TAG, "getWifiApConfiguration() returned null");
            return null;
        }

        String ssid     = apConfig.SSID         != null ? apConfig.SSID         : "";
        String password = apConfig.preSharedKey  != null ? apConfig.preSharedKey  : "";
        String[] netInfo = getApNetworkInfo();
        String ip    = netInfo[0];
        String bssid = netInfo[1];

        Log.i(TAG, "Hotspot config: ssid=" + ssid + " ip=" + ip
            + " bssid=" + bssid + " port=" + TCP_PORT);
        return new BluetoothWirelessManager.HotspotConfig(ssid, password, bssid, ip, TCP_PORT);
    }

    private String[] getApNetworkInfo() {
        for (int attempt = 0; attempt < 10; attempt++) {
            try {
                for (NetworkInterface iface :
                        Collections.list(NetworkInterface.getNetworkInterfaces())) {
                    if (iface.isLoopback() || !iface.isUp()) continue;
                    String name = iface.getName();
                    if (name.startsWith("wlan") || name.startsWith("ap")
                            || name.startsWith("swlan") || name.startsWith("p2p")) {
                        for (InetAddress addr :
                                Collections.list(iface.getInetAddresses())) {
                            if (addr instanceof Inet4Address && !addr.isLoopbackAddress()) {
                                String ip = addr.getHostAddress();
                                byte[] mac = iface.getHardwareAddress();
                                String bssid = "02:00:00:00:00:00";
                                if (mac != null) {
                                    bssid = String.format("%02x:%02x:%02x:%02x:%02x:%02x",
                                        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                                }
                                Log.i(TAG, "AP interface " + name
                                    + " -> ip=" + ip + " bssid=" + bssid
                                    + " (attempt " + (attempt + 1) + ")");
                                return new String[]{ ip, bssid };
                            }
                        }
                    }
                }
            } catch (Exception e) {
                Log.w(TAG, "getApNetworkInfo failed: " + e.getMessage());
            }
            Log.i(TAG, "AP interface not ready, waiting 500ms (attempt " + (attempt + 1) + ")");
            try { Thread.sleep(500); } catch (InterruptedException ignored) { break; }
        }
        Log.w(TAG, "Timeout waiting for AP interface, using fallback");
        return new String[]{ "192.168.43.1", "02:00:00:00:00:00" };
    }

    private Notification buildNotification() {
        NotificationChannel channel = new NotificationChannel(
                NOTIFICATION_CHANNEL, "Android Auto Wireless",
                NotificationManager.IMPORTANCE_MIN);
        channel.setShowBadge(false);
        getSystemService(NotificationManager.class).createNotificationChannel(channel);
        return new Notification.Builder(this, NOTIFICATION_CHANNEL)
                .setContentTitle("Android Auto Wireless")
                .setSmallIcon(android.R.drawable.ic_menu_info_details)
                .setOngoing(true)
                .setShowWhen(false)
                .build();
    }
}
