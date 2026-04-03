package com.aauto.app.usb;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.util.Log;

import com.aauto.app.BuildInfo;
import com.aauto.app.core.AaSessionService;

/**
 * Foreground service that manages the full USB/AOA lifecycle.
 *
 * Started on boot via BootReceiver. Registers a dynamic BroadcastReceiver for
 * USB_DEVICE_ATTACHED and USB_DEVICE_DETACHED. Holds the UsbDeviceConnection
 * alive after handing off the fd to AaSessionService so the fd stays valid
 * for the duration of the AA session.
 *
 * When a device is ready: sends ACTION_USB_DEVICE_READY to AaSessionService.
 * AaSessionService starts the AAP session and broadcasts ACTION_DEVICE_LIST_CHANGED.
 * MainActivity receives the broadcast and shows the device in the list.
 * The user then selects the device to launch AaDisplayActivity.
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
        Log.i(TAG, "AOA confirmed, notifying AaSessionService: fd=" + fd + " id=" + deviceId);
        Intent intent = new Intent(this, AaSessionService.class);
        intent.setAction(AaSessionService.ACTION_USB_DEVICE_READY);
        intent.putExtra(AaSessionService.EXTRA_USB_FD, fd);
        intent.putExtra(AaSessionService.EXTRA_USB_EP_IN, epIn);
        intent.putExtra(AaSessionService.EXTRA_USB_EP_OUT, epOut);
        intent.putExtra(AaSessionService.EXTRA_DEVICE_ID, deviceId);
        startForegroundService(intent);
    }

    @Override
    public void onDeviceDetached(String deviceId) {
        Log.i(TAG, "AOA device detached: " + deviceId);
        Intent intent = new Intent(this, AaSessionService.class);
        intent.setAction(AaSessionService.ACTION_USB_DEVICE_DETACHED);
        intent.putExtra(AaSessionService.EXTRA_DEVICE_ID, deviceId);
        startForegroundService(intent);
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
