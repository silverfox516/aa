package com.aauto.app;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

import com.aauto.app.core.AaSessionService;
import com.aauto.app.usb.UsbMonitorService;
import com.aauto.app.wireless.WirelessMonitorService;

public class BootReceiver extends BroadcastReceiver {

    private static final String TAG = "AA.APP.BootReceiver";

    @Override
    public void onReceive(Context context, Intent intent) {
        if (Intent.ACTION_BOOT_COMPLETED.equals(intent.getAction())) {
            Log.i(TAG, "Boot completed, starting services");
            context.startForegroundService(new Intent(context, AaSessionService.class));
            context.startForegroundService(new Intent(context, UsbMonitorService.class));
            context.startForegroundService(new Intent(context, WirelessMonitorService.class));
        }
    }
}
