package com.aauto.app;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.ServiceConnection;
import android.os.Bundle;
import android.os.IBinder;
import android.util.Log;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;

import com.aauto.app.core.AaSessionService;

/**
 * Full-screen Android Auto display activity.
 *
 * This activity is a thin view layer — it owns no session state.
 * It binds to AaSessionService and delegates:
 *   - Surface lifecycle  → service.onSurfaceReady() / onSurfaceDestroyed()
 *   - Touch events       → service.dispatchTouchEvent()
 *
 * Started by MainActivity when the user selects a CONNECTED device.
 * Finishes automatically when ACTION_SESSION_ENDED is received for its deviceId.
 */
public class AaDisplayActivity extends Activity implements SurfaceHolder.Callback {

    private static final String TAG = "AA.AaDisplayActivity";

    /** The device session this activity is displaying. Provided by MainActivity. */
    public static final String EXTRA_DEVICE_ID = "device_id";

    private String         deviceId_   = null;
    private SurfaceView    surfaceView_;

    // ─── Service binding ─────────────────────────────────────────────────────

    private AaSessionService service_     = null;
    private boolean          bound_       = false;

    /**
     * Pending surface: stored when the surface is ready before the bind completes.
     * Delivered to the service in onServiceConnected.
     */
    private Surface pendingSurface_ = null;
    private int     pendingWidth_   = 0;
    private int     pendingHeight_  = 0;

    private final ServiceConnection serviceConnection_ = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName name, IBinder binder) {
            service_ = ((AaSessionService.LocalBinder) binder).getService();
            bound_   = true;
            Log.i(TAG, "Service connected");
            if (pendingSurface_ != null) {
                service_.onSurfaceReady(deviceId_, pendingSurface_, pendingWidth_, pendingHeight_);
                pendingSurface_ = null;
            }
        }

        @Override
        public void onServiceDisconnected(ComponentName name) {
            service_ = null;
            bound_   = false;
            Log.w(TAG, "Service disconnected");
        }
    };

    // ─── Session end receiver ─────────────────────────────────────────────────

    private final BroadcastReceiver sessionEndReceiver_ = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            String endedId = intent.getStringExtra(AaSessionService.EXTRA_SESSION_ENDED_ID);
            if (deviceId_ != null && deviceId_.equals(endedId)) {
                Log.i(TAG, "Session ended for device=" + deviceId_ + ", finishing");
                finish();
            }
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
            dispatchTouchToService(event);
            return true;
        });

        deviceId_ = getIntent().getStringExtra(EXTRA_DEVICE_ID);
        Log.i(TAG, "onCreate deviceId=" + deviceId_);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        String newId = intent.getStringExtra(EXTRA_DEVICE_ID);
        if (newId != null && !newId.equals(deviceId_)) {
            // Switching to a different device — surface lifecycle will handle focus transitions.
            deviceId_ = newId;
            Log.i(TAG, "onNewIntent: switching to deviceId=" + deviceId_);
        }
    }

    @Override
    protected void onStart() {
        super.onStart();
        Intent serviceIntent = new Intent(this, AaSessionService.class);
        bindService(serviceIntent, serviceConnection_, Context.BIND_AUTO_CREATE);
        registerReceiver(sessionEndReceiver_,
            new IntentFilter(AaSessionService.ACTION_SESSION_ENDED));
    }

    @Override
    protected void onStop() {
        super.onStop();
        unregisterReceiver(sessionEndReceiver_);
        if (bound_) {
            unbindService(serviceConnection_);
            bound_   = false;
            service_ = null;
        }
    }

    // ─── SurfaceHolder.Callback ───────────────────────────────────────────────

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        Log.i(TAG, "surfaceCreated");
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        Log.i(TAG, "surfaceChanged: " + width + "x" + height);
        Surface surface = holder.getSurface();
        if (bound_ && service_ != null) {
            service_.onSurfaceReady(deviceId_, surface, width, height);
        } else {
            pendingSurface_ = surface;
            pendingWidth_   = width;
            pendingHeight_  = height;
        }
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        Log.i(TAG, "surfaceDestroyed");
        pendingSurface_ = null;
        if (bound_ && service_ != null) {
            service_.onSurfaceDestroyed(deviceId_);
        }
    }

    // ─── Touch forwarding ─────────────────────────────────────────────────────

    private void dispatchTouchToService(MotionEvent event) {
        if (!bound_ || service_ == null) return;
        int idx = event.getActionIndex();
        service_.dispatchTouchEvent(
            event.getPointerId(idx),
            event.getX(idx),
            event.getY(idx),
            event.getActionMasked());
    }
}
