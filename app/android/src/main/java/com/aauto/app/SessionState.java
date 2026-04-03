package com.aauto.app;

/**
 * Tracks the currently active AA session so MainActivity can reflect
 * connection state and decide whether to start a new session or bring
 * AaDisplayActivity back to front.
 */
public class SessionState {

    public enum ConnectionType { NONE, USB, WIRELESS }

    private static ConnectionType activeType_   = ConnectionType.NONE;
    private static String         activeDeviceId_ = null;

    public static synchronized void setActive(ConnectionType type, String deviceId) {
        activeType_    = type;
        activeDeviceId_ = deviceId;
    }

    public static synchronized void clearActive() {
        activeType_    = ConnectionType.NONE;
        activeDeviceId_ = null;
    }

    public static synchronized ConnectionType getActiveType() { return activeType_; }
    public static synchronized String getActiveDeviceId()     { return activeDeviceId_; }
    public static synchronized boolean isConnected()          { return activeType_ != ConnectionType.NONE; }
}
