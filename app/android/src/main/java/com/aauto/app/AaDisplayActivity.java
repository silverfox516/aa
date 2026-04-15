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
import android.widget.Button;

import com.aauto.app.core.AaSessionService;

/**
 * Full-screen Android Auto display activity.
 *
 * Owns no session state. Holds the system rendering Surface and forwards
 * touch events. AaSessionService routes the Surface to whichever session is
 * currently active — selection is performed by MainActivity before this
 * activity is started.
 *
 * Lifecycle:
 *   onStart  → bind to AaSessionService
 *   surface  → service.onSurfaceReady / onSurfaceDestroyed
 *   onStop   → service.deactivateAll() (releases sinks for all sessions)
 *              + unbind
 *
 * Finishes automatically when ACTION_SESSION_ENDED is received and there is
 * no longer an active session, so the user lands back on MainActivity.
 *
 * Declared with android:noHistory="true" so swiping away or backing out
 * removes it from the recents/back stack — the only path back is MainActivity
 * → tap a session.
 */
public class AaDisplayActivity extends Activity implements SurfaceHolder.Callback {

    private static final String TAG = "AA.APP.AaDisplayActivity";

    private SurfaceView surfaceView_;

    // ─── Service binding ─────────────────────────────────────────────────────

    private AaSessionService service_ = null;
    private boolean          bound_   = false;

    /** Pending surface stored when ready before the bind completes. */
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
                service_.onSurfaceReady(pendingSurface_, pendingWidth_, pendingHeight_);
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

    // ─── Session end receiver ────────────────────────────────────────────────

    private final BroadcastReceiver sessionEndReceiver_ = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            // Only finish if the active session is gone — an inactive session
            // ending should not kick the user out of the display they chose.
            if (bound_ && service_ != null && service_.getActiveHandle() != 0) {
                return;
            }
            Log.i(TAG, "Active session ended, finishing");
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
            dispatchTouchToService(event);
            return true;
        });

        // Floating "back to launcher" overlay. Tapping it finishes this
        // activity; because AaDisplayActivity is declared android:noHistory
        // it pops off the back stack and the user lands on MainActivity.
        Button backButton = findViewById(R.id.btn_back_home);
        if (backButton != null) {
            backButton.setOnClickListener(v -> {
                Log.i(TAG, "Back-to-launcher tapped, finishing");
                finish();
            });
        }

        Log.i(TAG, "onCreate");
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
        // Do NOT call deactivateAll() here. The active session's audio should
        // keep playing when the user leaves this activity (background audio).
        // Video is detached automatically by surfaceDestroyed → onSurfaceDestroyed,
        // which transitions the session to BACKGROUND_AUDIO.
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
            service_.onSurfaceReady(surface, width, height);
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
            service_.onSurfaceDestroyed();
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
