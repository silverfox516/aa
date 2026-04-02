package com.aauto.app.usb;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.util.HashMap;

/**
 * Manages USB device detection and Android Open Accessory (AOA) protocol switching.
 *
 * Flow:
 *   1. USB_DEVICE_ATTACHED received
 *   2. Request USB permission
 *   3. On permission granted: attempt AOA protocol negotiation
 *   4. If device supports AOA: send accessory strings + AOA_START
 *   5. Device re-enumerates with AOA PID (0x2D00..0x2D05)
 *   6. Open AOA device, get file descriptor, pass to native layer
 */
public class UsbAccessoryManager {

    private static final String TAG = "AA.UsbAccessory";
    private static final String ACTION_USB_PERMISSION = "com.aauto.app.USB_PERMISSION";

    // Google VID for AOA devices
    private static final int GOOGLE_VID = 0x18D1;
    private static final int AOA_PID_MIN = 0x2D00;
    private static final int AOA_PID_MAX = 0x2D05;

    // AOA control request codes
    private static final int AOA_GET_PROTOCOL  = 51;
    private static final int AOA_SEND_STRING   = 52;
    private static final int AOA_START         = 53;

    // AOA accessory string indices
    private static final int AOA_STRING_MANUFACTURER = 0;
    private static final int AOA_STRING_MODEL        = 1;
    private static final int AOA_STRING_DESCRIPTION  = 2;
    private static final int AOA_STRING_VERSION      = 3;
    private static final int AOA_STRING_URI          = 4;
    private static final int AOA_STRING_SERIAL       = 5;

    // USB control transfer constants
    private static final int USB_DIR_IN  = UsbConstants.USB_DIR_IN;
    private static final int USB_DIR_OUT = UsbConstants.USB_DIR_OUT;
    private static final int USB_TYPE_VENDOR = 0x40;
    private static final int USB_TIMEOUT_MS  = 1000;

    public interface Listener {
        /** Called when an AOA device is ready. fd, epIn, epOut for bulk I/O. */
        void onDeviceReady(int fd, int epIn, int epOut, String deviceId);
        /** Called when the AOA device is detached or the connection is lost. */
        void onDeviceDetached(String deviceId);
    }

    private final Context     context_;
    private final UsbManager  usbManager_;
    private final Listener    listener_;
    private final Handler     mainHandler_;

    // Currently open AOA connection (only one at a time for now)
    private UsbDeviceConnection activeConnection_ = null;
    private String              activeDeviceId_   = null;

    private final BroadcastReceiver receiver_ = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            final String action = intent.getAction();
            if (action == null) return;

            switch (action) {
                case UsbManager.ACTION_USB_DEVICE_ATTACHED:
                    handleDeviceAttached(intent.getParcelableExtra(UsbManager.EXTRA_DEVICE));
                    break;
                case UsbManager.ACTION_USB_DEVICE_DETACHED:
                    handleDeviceDetached(intent.getParcelableExtra(UsbManager.EXTRA_DEVICE));
                    break;
                case ACTION_USB_PERMISSION:
                    handlePermissionResult(intent);
                    break;
            }
        }
    };

    public UsbAccessoryManager(Context context, Listener listener) {
        context_     = context.getApplicationContext();
        usbManager_  = (UsbManager) context_.getSystemService(Context.USB_SERVICE);
        listener_    = listener;
        mainHandler_ = new Handler(Looper.getMainLooper());
    }

    public void start() {
        IntentFilter filter = new IntentFilter();
        filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        filter.addAction(ACTION_USB_PERMISSION);
        context_.registerReceiver(receiver_, filter);

        // Check devices already connected at startup
        scanConnectedDevices();
    }

    public void stop() {
        try {
            context_.unregisterReceiver(receiver_);
        } catch (IllegalArgumentException e) {
            // Receiver was not registered
        }
        closeActiveConnection();
    }

    // ─── Private helpers ──────────────────────────────────────────────────────

    private void scanConnectedDevices() {
        HashMap<String, UsbDevice> devices = usbManager_.getDeviceList();
        for (UsbDevice device : devices.values()) {
            handleDeviceAttached(device);
        }
    }

    private void handleDeviceAttached(UsbDevice device) {
        if (device == null) return;
        Log.i(TAG, "USB device attached: " + device.getDeviceName()
                + " VID=" + String.format("0x%04X", device.getVendorId())
                + " PID=" + String.format("0x%04X", device.getProductId()));

        onPermissionGranted(device);
    }

    private void onPermissionGranted(UsbDevice device) {
        if (isAoaDevice(device)) {
            new Thread(() -> openAoaDevice(device), "usb-open").start();
        } else {
            new Thread(() -> attemptAoaSwitch(device), "usb-aoa-switch").start();
        }
    }

    private void handleDeviceDetached(UsbDevice device) {
        if (device == null) return;
        final String id = device.getDeviceName();
        Log.i(TAG, "USB device detached: " + id);
        if (id.equals(activeDeviceId_)) {
            closeActiveConnection();
            listener_.onDeviceDetached(id);
        }
    }

    private void handlePermissionResult(Intent intent) {
        UsbDevice device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
        boolean granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false);
        if (device == null) return;

        if (granted) {
            Log.i(TAG, "USB permission granted for: " + device.getDeviceName());
            onPermissionGranted(device);
        } else {
            Log.w(TAG, "USB permission denied for: " + device.getDeviceName());
        }
    }

    /**
     * Attempts to switch a regular Android device into AOA accessory mode.
     * Runs on a background thread (uses blocking control transfers).
     */
    private void attemptAoaSwitch(UsbDevice device) {
        UsbDeviceConnection conn = usbManager_.openDevice(device);
        if (conn == null) {
            Log.w(TAG, "Could not open device for AOA negotiation: " + device.getDeviceName());
            return;
        }

        try {
            // Step 1: Get AOA protocol version
            byte[] versionBuf = new byte[2];
            int transferred = conn.controlTransfer(
                USB_DIR_IN | USB_TYPE_VENDOR,
                AOA_GET_PROTOCOL, 0, 0,
                versionBuf, 2, USB_TIMEOUT_MS);

            if (transferred < 2) {
                Log.d(TAG, "Device does not support AOA: " + device.getDeviceName());
                return;
            }
            int version = (versionBuf[1] << 8) | (versionBuf[0] & 0xFF);
            if (version < 1) {
                Log.d(TAG, "AOA version not supported: " + version);
                return;
            }
            Log.i(TAG, "AOA protocol version: " + version);

            // Step 2: Send accessory identification strings
            sendAoaString(conn, AOA_STRING_MANUFACTURER, "Android");
            sendAoaString(conn, AOA_STRING_MODEL,        "Android Auto");
            sendAoaString(conn, AOA_STRING_DESCRIPTION,  "Android Auto");
            sendAoaString(conn, AOA_STRING_VERSION,      "2.0.1");
            sendAoaString(conn, AOA_STRING_URI,          "https://developer.android.com/auto/index.html");
            sendAoaString(conn, AOA_STRING_SERIAL,       "HU-AAAAAA001");

            // Step 3: Send AOA_START — device will re-enumerate
            conn.controlTransfer(
                USB_DIR_OUT | USB_TYPE_VENDOR,
                AOA_START, 0, 0,
                null, 0, USB_TIMEOUT_MS);

            Log.i(TAG, "AOA start sent. Device will re-enumerate.");
        } finally {
            conn.close();
        }
    }

    private void sendAoaString(UsbDeviceConnection conn, int index, String value) {
        byte[] bytes = (value + "\0").getBytes();
        conn.controlTransfer(
            USB_DIR_OUT | USB_TYPE_VENDOR,
            AOA_SEND_STRING, 0, index,
            bytes, bytes.length, USB_TIMEOUT_MS);
    }

    /**
     * Opens an AOA device, finds the bulk endpoints, and passes the file
     * descriptor to the native layer.
     */
    private void openAoaDevice(UsbDevice device) {
        // Find interface with exactly 2 bulk endpoints
        UsbInterface intf = findAoaInterface(device);
        if (intf == null) {
            Log.e(TAG, "No suitable bulk interface found on AOA device");
            return;
        }

        // Extract bulk IN and OUT endpoint addresses
        int epIn = -1, epOut = -1;
        for (int i = 0; i < intf.getEndpointCount(); i++) {
            UsbEndpoint ep = intf.getEndpoint(i);
            if (ep.getType() == UsbConstants.USB_ENDPOINT_XFER_BULK) {
                if (ep.getDirection() == UsbConstants.USB_DIR_IN) epIn  = ep.getAddress();
                else                                                epOut = ep.getAddress();
            }
        }
        if (epIn == -1 || epOut == -1) {
            Log.e(TAG, "Could not find both bulk endpoints");
            return;
        }

        UsbDeviceConnection conn = usbManager_.openDevice(device);
        if (conn == null) {
            Log.e(TAG, "Failed to open AOA device");
            return;
        }

        if (!conn.claimInterface(intf, true)) {
            Log.e(TAG, "Failed to claim AOA interface");
            conn.close();
            return;
        }

        int fd = conn.getFileDescriptor();
        String deviceId = device.getDeviceName();

        Log.i(TAG, "AOA device opened. fd=" + fd
                + " ep_in=0x" + Integer.toHexString(epIn)
                + " ep_out=0x" + Integer.toHexString(epOut)
                + " id=" + deviceId);

        synchronized (this) {
            closeActiveConnection();
            activeConnection_ = conn;
            activeDeviceId_   = deviceId;
        }

        listener_.onDeviceReady(fd, epIn, epOut, deviceId);
    }

    private UsbInterface findAoaInterface(UsbDevice device) {
        for (int i = 0; i < device.getInterfaceCount(); i++) {
            UsbInterface intf = device.getInterface(i);
            if (intf.getEndpointCount() == 2) {
                UsbEndpoint ep0 = intf.getEndpoint(0);
                UsbEndpoint ep1 = intf.getEndpoint(1);
                boolean hasBulkIn  = (ep0.getType() == UsbConstants.USB_ENDPOINT_XFER_BULK && ep0.getDirection() == UsbConstants.USB_DIR_IN)
                                  || (ep1.getType() == UsbConstants.USB_ENDPOINT_XFER_BULK && ep1.getDirection() == UsbConstants.USB_DIR_IN);
                boolean hasBulkOut = (ep0.getType() == UsbConstants.USB_ENDPOINT_XFER_BULK && ep0.getDirection() == UsbConstants.USB_DIR_OUT)
                                  || (ep1.getType() == UsbConstants.USB_ENDPOINT_XFER_BULK && ep1.getDirection() == UsbConstants.USB_DIR_OUT);
                if (hasBulkIn && hasBulkOut) return intf;
            }
        }
        return null;
    }

    private boolean isAoaDevice(UsbDevice device) {
        return device.getVendorId() == GOOGLE_VID
            && device.getProductId() >= AOA_PID_MIN
            && device.getProductId() <= AOA_PID_MAX;
    }

    private synchronized void closeActiveConnection() {
        if (activeConnection_ != null) {
            activeConnection_.close();
            activeConnection_ = null;
            activeDeviceId_   = null;
        }
    }
}
