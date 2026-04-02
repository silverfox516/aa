package com.aauto.app;

import android.app.Activity;
import com.aauto.app.BuildInfo;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Bundle;
import android.util.Log;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;

/**
 * Main activity for the Android Auto head unit application.
 *
 * This activity is started by UsbHandlerActivity only after an AOA device is
 * confirmed. It does not handle USB detection — that is UsbHandlerActivity's job.
 *
 * Responsibilities:
 *   - Host a full-screen SurfaceView for video rendering
 *   - Bridge JNI engine lifecycle to Activity lifecycle
 *   - Forward touch events to the native input service
 *   - Stop and finish when USB device is detached
 */
public class MainActivity extends Activity implements SurfaceHolder.Callback {

    private static final String TAG = "AA.MainActivity";

    public static final String EXTRA_USB_FD        = "usb_fd";
    public static final String EXTRA_USB_EP_IN     = "usb_ep_in";
    public static final String EXTRA_USB_EP_OUT    = "usb_ep_out";
    public static final String EXTRA_USB_DEVICE_ID = "usb_device_id";
    public static final String ACTION_USB_DETACHED  = "com.aauto.app.ACTION_USB_DETACHED";

    static {
        System.loadLibrary("aauto_jni");
    }

    private SurfaceView surfaceView_;

    private final BroadcastReceiver detachReceiver_ = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            Log.i(TAG, "USB detached, stopping AA");
            nativeOnUsbDeviceDetached("");
            finish();
        }
    };

    // ─── Activity lifecycle ───────────────────────────────────────────────────

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        getWindow().getDecorView().setSystemUiVisibility(
            View.SYSTEM_UI_FLAG_FULLSCREEN
            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
            | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);

        setContentView(R.layout.activity_main);

        surfaceView_ = findViewById(R.id.surface_view);
        surfaceView_.getHolder().addCallback(this);
        surfaceView_.setOnTouchListener((v, event) -> {
            dispatchTouchToNative(event);
            return true;
        });

        Log.i(TAG, "onCreate [build " + BuildInfo.BUILD_VERSION + "]");
        nativeInit(null);

        int fd = getIntent().getIntExtra(EXTRA_USB_FD, -1);
        int epIn = getIntent().getIntExtra(EXTRA_USB_EP_IN, 0);
        int epOut = getIntent().getIntExtra(EXTRA_USB_EP_OUT, 0);
        String deviceId = getIntent().getStringExtra(EXTRA_USB_DEVICE_ID);
        if (fd != -1) {
            Log.i(TAG, "USB fd received at start: fd=" + fd + " id=" + deviceId);
            nativeOnUsbDeviceReady(fd, epIn, epOut, deviceId != null ? deviceId : "");
        }
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        int fd = intent.getIntExtra(EXTRA_USB_FD, -1);
        int epIn = intent.getIntExtra(EXTRA_USB_EP_IN, 0);
        int epOut = intent.getIntExtra(EXTRA_USB_EP_OUT, 0);
        String deviceId = intent.getStringExtra(EXTRA_USB_DEVICE_ID);
        if (fd != -1) {
            Log.i(TAG, "USB fd re-delivered: fd=" + fd + " id=" + deviceId);
            nativeOnUsbDeviceReady(fd, epIn, epOut, deviceId != null ? deviceId : "");
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        registerReceiver(detachReceiver_, new IntentFilter(ACTION_USB_DETACHED));
        nativeStart();
    }

    @Override
    protected void onPause() {
        super.onPause();
        unregisterReceiver(detachReceiver_);
        nativeStop();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        nativeDestroy();
    }

    // ─── SurfaceHolder.Callback ───────────────────────────────────────────────

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        Log.i(TAG, "Surface created");
        nativeSetSurface(holder.getSurface());
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        Log.i(TAG, "Surface changed: " + width + "x" + height);
        nativeSetViewSize(width, height);
        nativeSetSurface(holder.getSurface());
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        Log.i(TAG, "Surface destroyed");
        nativeSetSurface(null);
    }

    // ─── Touch forwarding ─────────────────────────────────────────────────────

    private void dispatchTouchToNative(MotionEvent event) {
        int action = event.getActionMasked();
        int idx    = event.getActionIndex();
        nativeOnTouchEvent(
            event.getPointerId(idx),
            event.getX(idx),
            event.getY(idx),
            action);
    }

    // ─── Native methods ───────────────────────────────────────────────────────

    private native void nativeInit(Surface surface);
    private native void nativeStart();
    private native void nativeStop();
    private native void nativeDestroy();
    private native void nativeSetSurface(Surface surface);
    private native void nativeOnUsbDeviceReady(int fd, int epIn, int epOut, String deviceId);
    private native void nativeOnUsbDeviceDetached(String deviceId);
    private native void nativeSetViewSize(int width, int height);
    private native void nativeOnTouchEvent(int pointerId, float x, float y, int action);
}
