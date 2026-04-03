package com.aauto.app.wireless;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.util.Log;

/**
 * Placeholder — full implementation in Stage 5.
 */
public class WirelessMonitorService extends Service {

    private static final String TAG                  = "AA.WirelessMonitorSvc";
    private static final String NOTIFICATION_CHANNEL = "aa_wireless_monitor";
    private static final int    NOTIFICATION_ID      = 2;

    @Override
    public void onCreate() {
        super.onCreate();
        startForeground(NOTIFICATION_ID, buildNotification());
        Log.i(TAG, "WirelessMonitorService started (stub)");
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
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
