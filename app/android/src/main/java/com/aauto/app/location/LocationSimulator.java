package com.aauto.app.location;

import android.os.Handler;
import android.os.Looper;
import android.util.Log;

/**
 * Drives a hardcoded route as a 1 Hz callback stream.
 *
 * Why this exists: this head unit has no GPS chipset, so the AAP location
 * sensor would otherwise have nothing to send. The simulator emits fixes
 * directly into a callback rather than going through Android's mock
 * LocationProvider — that path requires AppOps OP_MOCK_LOCATION to be
 * granted by system service, which is brittle across builds even for
 * privileged apps. The trade-off is that the simulator and the real
 * LocationManager listener are now wired into the same callback at the
 * AaSessionService layer instead of sharing a single LocationManager
 * subscription, but the application code that consumes the fix is still
 * unified at the callback boundary.
 *
 * On platforms where a real GPS chipset is present, this class is simply
 * not started; the LocationManager listener handles it.
 *
 * Hardcoded route: a small loop in central Seoul (Gangnam Station area).
 */
public class LocationSimulator {

    private static final String TAG = "AA.APP.LocationSimulator";

    /** Tick period — matches typical 1 Hz GPS fix rate. */
    private static final long TICK_PERIOD_MS = 1000L;

    /** Constant simulated speed: 100 km/h ≈ 27.78 m/s. */
    private static final float CRUISE_SPEED_MPS = 27.78f;

    /** Cruising altitude (Seoul ~40m). */
    private static final double DEFAULT_ALT_M = 40.0;

    /** Constant simulated horizontal accuracy. */
    private static final float DEFAULT_ACC_M = 5.0f;

    /**
     * Hardcoded loop following real roads. Pairs are stored as
     * {longitude, latitude} in WGS84 degrees — this is the order returned
     * directly by OSRM / GeoJSON, so the polyline can be pasted in
     * verbatim from the routing API. The simulator interpolates linearly
     * between consecutive points at the cruise speed and wraps back to
     * start.
     *
     * Source: router.project-osrm.org driving route, simplified geometry,
     *   서울 시청 → 부산 시청 → 서울 시청 (round trip)
     * (≈ 800 km, ≈ 8 h at 100 km/h).
     */
    private static final double[][] LOOP_LONLAT = {
        {126.978407,37.567005},{127.042047,37.572614},{127.138839,37.533806},{127.177612,37.54811},
        {127.216931,37.529752},{127.2564,37.45597},{127.295585,37.431994},{127.327619,37.336731},
        {127.426604,37.234358},{127.482187,37.245999},{127.588097,37.231838},{127.660686,37.129176},
        {127.71218,37.106783},{127.732375,37.047048},{127.841672,37.01941},{127.84898,36.94322},
        {127.887568,36.913334},{127.8985,36.8767},{127.95324,36.84445},{127.985354,36.764003},
        {128.078864,36.740798},{128.143217,36.649306},{128.162631,36.598909},{128.146446,36.544896},
        {128.240248,36.393607},{128.220422,36.315503},{128.257199,36.26898},{128.264945,36.159186},
        {128.35635,36.121412},{128.36043,36.073018},{128.39858,36.066656},{128.430183,36.020154},
        {128.42975,35.980483},{128.497933,35.895665},{128.646432,35.916507},{128.690704,35.887551},
        {128.690133,35.839132},{128.730724,35.806806},{128.745862,35.656632},{128.79154,35.54616},
        {128.783926,35.427707},{128.856999,35.363981},{128.910684,35.35861},{128.968051,35.309555},
        {128.985052,35.281604},{128.973179,35.218326},{129.069646,35.207952},{129.076164,35.179296},
        {129.069359,35.208114},{128.976245,35.220142},{128.985048,35.283033},{128.966995,35.310992},
        {128.910668,35.358726},{128.857729,35.363885},{128.784293,35.42726},{128.791944,35.545802},
        {128.745969,35.656667},{128.730839,35.806823},{128.689887,35.840154},{128.692029,35.888013},
        {128.646242,35.916678},{128.497948,35.895872},{128.429987,35.980427},{128.430389,36.020134},
        {128.398346,36.06752},{128.36064,36.073057},{128.356364,36.121646},{128.266839,36.161603},
        {128.257306,36.269006},{128.220634,36.315528},{128.240508,36.393853},{128.146611,36.544777},
        {128.162754,36.59919},{128.14334,36.64938},{128.078926,36.740904},{127.985802,36.7637},
        {127.95333,36.84478},{127.89874,36.87679},{127.88782,36.9134},{127.84912,36.94369},
        {127.841788,37.019481},{127.73244,37.04716},{127.71229,37.10686},{127.659983,37.130057},
        {127.589236,37.234044},{127.482133,37.246178},{127.432486,37.234667},{127.348175,37.25199},
        {127.288538,37.239183},{127.229565,37.249016},{127.160115,37.287224},{127.105168,37.290959},
        {127.100115,37.400323},{127.029936,37.474252},{126.978407,37.567005},
    };

    // Indices into a {longitude, latitude} pair.
    private static final int LON = 0;
    private static final int LAT = 1;

    /**
     * One simulated GPS fix. Same shape as the parameters consumed by
     * AaSessionService.nativeSendLocation, except in floating point so the
     * caller can do its own scaling.
     */
    public interface FixListener {
        void onFix(double lat, double lon, double altMeters,
                   float accuracyMeters, float speedMps, float bearingDeg,
                   long unixTimeMs);
    }

    private final FixListener listener_;
    private final Handler     handler_ = new Handler(Looper.getMainLooper());

    private boolean running_ = false;
    private int     segIndex_ = 0;       // current segment start index
    private double  segProgress_ = 0.0;  // 0..1 along the current segment
    private double  segLengthM_ = 0.0;   // cached segment length in meters

    public LocationSimulator(FixListener listener) {
        listener_ = listener;
    }

    public void start() {
        segIndex_ = 0;
        segProgress_ = 0.0;
        segLengthM_ = haversineMeters(LOOP_LONLAT[0], LOOP_LONLAT[1]);
        running_ = true;
        handler_.post(tickRunnable_);
        Log.i(TAG, "Started — Seoul↔Busan loop, " + LOOP_LONLAT.length + " points, "
                   + CRUISE_SPEED_MPS + " m/s");
    }

    public void stop() {
        running_ = false;
        handler_.removeCallbacks(tickRunnable_);
        Log.i(TAG, "Stopped");
    }

    private final Runnable tickRunnable_ = new Runnable() {
        @Override
        public void run() {
            if (!running_) return;
            advanceAndEmit();
            handler_.postDelayed(this, TICK_PERIOD_MS);
        }
    };

    private void advanceAndEmit() {
        // Advance the position by (cruise speed × tick period) meters along
        // the current segment, wrapping to the next segment when needed.
        double advanceM = CRUISE_SPEED_MPS * (TICK_PERIOD_MS / 1000.0);
        while (advanceM > 0 && running_) {
            double remainingM = (1.0 - segProgress_) * segLengthM_;
            if (advanceM < remainingM) {
                segProgress_ += advanceM / segLengthM_;
                advanceM = 0;
            } else {
                advanceM -= remainingM;
                segIndex_ = (segIndex_ + 1) % LOOP_LONLAT.length;
                int next = (segIndex_ + 1) % LOOP_LONLAT.length;
                segLengthM_ = haversineMeters(LOOP_LONLAT[segIndex_], LOOP_LONLAT[next]);
                // Adjacent OSRM points can be sub-meter apart; skip them so
                // we don't divide-by-zero on segLengthM.
                if (segLengthM_ < 1e-3) segLengthM_ = 1e-3;
                segProgress_ = 0.0;
            }
        }

        double[] a = LOOP_LONLAT[segIndex_];
        double[] b = LOOP_LONLAT[(segIndex_ + 1) % LOOP_LONLAT.length];
        double lat = a[LAT] + (b[LAT] - a[LAT]) * segProgress_;
        double lon = a[LON] + (b[LON] - a[LON]) * segProgress_;
        float bearing = (float) bearingDegrees(a, b);

        if (listener_ != null) {
            listener_.onFix(lat, lon, DEFAULT_ALT_M, DEFAULT_ACC_M,
                            CRUISE_SPEED_MPS, bearing,
                            System.currentTimeMillis());
        }
    }

    // Great-circle distance between two {lon, lat} pairs in meters.
    private static double haversineMeters(double[] a, double[] b) {
        double R = 6371000.0;
        double lat1 = Math.toRadians(a[LAT]);
        double lat2 = Math.toRadians(b[LAT]);
        double dLat = Math.toRadians(b[LAT] - a[LAT]);
        double dLon = Math.toRadians(b[LON] - a[LON]);
        double s = Math.sin(dLat / 2) * Math.sin(dLat / 2)
                 + Math.cos(lat1) * Math.cos(lat2)
                 * Math.sin(dLon / 2) * Math.sin(dLon / 2);
        return 2 * R * Math.asin(Math.sqrt(s));
    }

    // Initial bearing from a to b in degrees [0, 360). Inputs are {lon, lat}.
    private static double bearingDegrees(double[] a, double[] b) {
        double lat1 = Math.toRadians(a[LAT]);
        double lat2 = Math.toRadians(b[LAT]);
        double dLon = Math.toRadians(b[LON] - a[LON]);
        double y = Math.sin(dLon) * Math.cos(lat2);
        double x = Math.cos(lat1) * Math.sin(lat2)
                 - Math.sin(lat1) * Math.cos(lat2) * Math.cos(dLon);
        double br = Math.toDegrees(Math.atan2(y, x));
        return (br + 360.0) % 360.0;
    }
}
