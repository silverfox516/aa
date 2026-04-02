package com.aauto.app.usb;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.util.Log;

import com.aauto.app.BuildInfo;
import com.aauto.app.MainActivity;

/**
 * Foreground service that manages the full USB/AOA lifecycle.
 *
 * Started on boot via BootReceiver. Registers a dynamic BroadcastReceiver for
 * USB_DEVICE_ATTACHED and USB_DEVICE_DETACHED. Holds the UsbDeviceConnection
 * alive after handing off the fd to the native engine so the fd stays valid
 * for the duration of the AA session.
 */
public class UsbMonitorService extends Service implements UsbAccessoryManager.Listener {

    private static final String TAG                   = "AA.UsbMonitor";
    private static final String NOTIFICATION_CHANNEL  = "aa_usb_monitor";
    private static final int    NOTIFICATION_ID       = 1;

    private UsbAccessoryManager usbManager_;

    // ─── Service lifecycle ────────────────────────────────────────────────────

    @Override
    public void onCreate() {
        super.onCreate();
        Log.i(TAG, "onCreate [build " + BuildInfo.BUILD_VERSION + "]");
        startForeground(NOTIFICATION_ID, buildNotification());
        usbManager_ = new UsbAccessoryManager(this, this);
        usbManager_.start();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        Log.i(TAG, "onDestroy");
        usbManager_.stop();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    // ─── UsbAccessoryManager.Listener ────────────────────────────────────────

    @Override
    public void onDeviceReady(int fd, int epIn, int epOut, String deviceId) {
        Log.i(TAG, "AOA confirmed, starting MainActivity fd=" + fd + " id=" + deviceId);
        Intent intent = new Intent(this, MainActivity.class);
        intent.putExtra(MainActivity.EXTRA_USB_FD, fd);
        intent.putExtra(MainActivity.EXTRA_USB_EP_IN, epIn);
        intent.putExtra(MainActivity.EXTRA_USB_EP_OUT, epOut);
        intent.putExtra(MainActivity.EXTRA_USB_DEVICE_ID, deviceId);
        intent.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        startActivity(intent);
    }

    @Override
    public void onDeviceDetached(String deviceId) {
        Log.i(TAG, "AOA device detached: " + deviceId);
        Intent intent = new Intent(MainActivity.ACTION_USB_DETACHED);
        intent.setPackage(getPackageName());
        sendBroadcast(intent);
    }

    // ─── Private helpers ──────────────────────────────────────────────────────

    private Notification buildNotification() {
        NotificationChannel channel = new NotificationChannel(
                NOTIFICATION_CHANNEL, "Android Auto",
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
}
