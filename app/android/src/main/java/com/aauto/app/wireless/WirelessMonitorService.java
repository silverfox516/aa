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
import java.util.Collections;

/**
 * Foreground service that manages the Wireless Android Auto (AAW) lifecycle.
 *
 * Started on boot by BootReceiver. Reads the SoftAP config from the system
 * WifiManager, then pre-binds the TCP server socket and opens an RFCOMM server
 * socket, both waiting for the phone.
 *
 * Flow:
 *   1. SoftAP enabled → read hotspot config (SSID/PW/BSSID/IP)
 *   2. BT on → startListeningIfReady()
 *   3. startListeningIfReady():
 *      a. Pre-bind TCP port 5277 → tcpServerFd_ (port is open before handshake)
 *      b. Start RFCOMM server (BluetoothWirelessManager)
 *   4. Phone connects via RFCOMM → ACTION_WIRELESS_CONNECTING to AaSessionService
 *   5. Handshake OK → AaSessionService.onWirelessDeviceReady(deviceId, tcpServerFd_)
 *      - port 5277 was already bound, so phone can connect immediately
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

    // Pre-bound TCP server socket — created before RFCOMM handshake starts.
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
                Log.i(TAG, "Bluetooth turning off — stopping RFCOMM listener");
                if (wirelessManager_ != null) wirelessManager_.stop();
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
                Log.i(TAG, "SoftAP disabled");
                hotspotConfig_ = null;
            }
        }
    };

    // ─── Service lifecycle ────────────────────────────────────────────────────

    @Override
    public void onCreate() {
        super.onCreate();
        Log.i(TAG, "onCreate [build " + BuildInfo.BUILD_VERSION + "]");
        startForeground(NOTIFICATION_ID, buildNotification());

        // Bind to AaSessionService so we can call onWirelessDeviceReady() directly.
        Intent sessionIntent = new Intent(this, AaSessionService.class);
        startForegroundService(sessionIntent);
        bindService(sessionIntent, sessionConn_, Context.BIND_AUTO_CREATE);

        registerReceiver(btStateReceiver_,
            new IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED));
        registerReceiver(apStateReceiver_,
            new IntentFilter("android.net.wifi.WIFI_AP_STATE_CHANGED"));

        // Read config if SoftAP is already up
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

        // Transfer ownership of the pre-bound TCP fd to AaSessionService.
        FileDescriptor fd = tcpServerFd_;
        tcpServerFd_ = null;

        if (sessionService_ != null && fd != null) {
            sessionService_.onWirelessDeviceReady(deviceId, deviceName, fd);
        } else {
            Log.e(TAG, "Cannot transfer TCP fd: service=" + sessionService_ + " fd=" + fd);
            closeFd(fd);
        }

        // Pre-bind a fresh TCP socket for the next session WITHOUT touching
        // the RFCOMM listener — the existing RFCOMM client connection is now
        // in keep-alive mode and must stay open as a control channel for the
        // current session. Tearing it down causes the phone to re-initiate
        // the entire AAW handshake (duplicate session bug).
        rebindTcpServerFdOnly();
    }

    @Override
    public void onConnectionFailed(String deviceId, String reason) {
        Log.e(TAG, "Wireless connection failed for " + deviceId + ": " + reason);
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

        // Pre-bind TCP server socket BEFORE starting the RFCOMM server,
        // so port 5277 is open by the time the phone receives START_RESPONSE.
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

    /** Pre-bind a fresh TCP server fd without disturbing the RFCOMM listener. */
    private void rebindTcpServerFdOnly() {
        closeTcpServerFd();
        FileDescriptor fd = preBoundTcpSocket();
        if (fd == null) {
            Log.e(TAG, "TCP rebind failed — next wireless session will fail");
            return;
        }
        tcpServerFd_ = fd;
    }

    /** Creates and returns a socket already bound and listening on TCP_PORT, or null on error. */
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

    /**
     * Scans network interfaces for the AP's IPv4 address and real MAC (BSSID).
     * Polls up to 10 times (5 s total) because the interface may not have an
     * IP assigned immediately after SoftAP is enabled.
     * Returns String[2] = { ip, bssid }.
     */
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
                                    + " → ip=" + ip + " bssid=" + bssid
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
