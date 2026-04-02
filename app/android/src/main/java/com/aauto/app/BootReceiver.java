package com.aauto.app;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

import com.aauto.app.usb.UsbMonitorService;

public class BootReceiver extends BroadcastReceiver {

    private static final String TAG = "AA.BootReceiver";

    @Override
    public void onReceive(Context context, Intent intent) {
        if (Intent.ACTION_BOOT_COMPLETED.equals(intent.getAction())) {
            Log.i(TAG, "Boot completed, starting UsbMonitorService");
            context.startForegroundService(new Intent(context, UsbMonitorService.class));
        }
    }
}
