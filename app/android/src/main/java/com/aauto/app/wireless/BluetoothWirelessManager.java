package com.aauto.app.wireless;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothServerSocket;
import android.bluetooth.BluetoothSocket;
import android.content.Context;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.UUID;

/**
 * Handles the Wireless Android Auto (AAW) Bluetooth RFCOMM lifecycle.
 *
 * HU acts as RFCOMM server: listens on the AAW UUID and waits for the phone
 * to initiate the connection. After the AAW handshake completes the RFCOMM
 * socket stays open as a keep-alive / control channel for the duration of
 * the wireless AA session.
 *
 * Thread model:
 *   listenThread_    — RFCOMM accept loop (one connection at a time)
 *   sessionThread_   — handshake + keep-alive for the current connection
 *
 * Socket ownership: the accepted BluetoothSocket is owned exclusively by
 * sessionThread_. listenThread_ hands it off at accept time and never
 * touches it again. This eliminates the race condition where a second
 * accept would close the socket out from under the first session.
 */
public class BluetoothWirelessManager {

    private static final String TAG = "AA.APP.BtWirelessMgr";

    private static final UUID AAW_UUID =
        UUID.fromString("4DE17A00-52CB-11E6-BDF4-0800200C9A66");

    // AAW RFCOMM message IDs
    private static final int MSG_WIFI_START_REQUEST      = 1;
    private static final int MSG_WIFI_INFO_REQUEST       = 2;
    private static final int MSG_WIFI_INFO_RESPONSE      = 3;
    private static final int MSG_WIFI_VERSION_REQUEST    = 4;
    private static final int MSG_WIFI_VERSION_RESPONSE   = 5;
    private static final int MSG_WIFI_CONNECTION_STATUS  = 6;
    private static final int MSG_WIFI_START_RESPONSE     = 7;

    private static final int SECURITY_WPA2_PERSONAL = 5;
    private static final int AP_TYPE_STATIC = 0;

    // ─── Hotspot config ──────────────────────────────────────────────────────

    static class HotspotConfig {
        final String ssid;
        final String password;
        final String bssid;
        final String ip;
        final int    port;

        HotspotConfig(String ssid, String password, String bssid, String ip, int port) {
            this.ssid     = ssid;
            this.password = password;
            this.bssid    = bssid;
            this.ip       = ip;
            this.port     = port;
        }
    }

    // ─── Listener ────────────────────────────────────────────────────────────

    interface Listener {
        void onDeviceConnecting(String deviceId, String deviceName);
        void onDeviceReady(String deviceId, String deviceName);
        void onConnectionFailed(String deviceId, String reason);
        /** RFCOMM session ended (peer close, IO error, or interrupt). */
        void onDeviceDisconnected(String deviceId, String reason);
    }

    // ─── Fields ──────────────────────────────────────────────────────────────

    private final Context       context_;
    private final Listener      listener_;
    private final HotspotConfig hotspot_;

    private volatile BluetoothServerSocket serverSocket_;
    private volatile Thread                listenThread_;
    private volatile Thread                sessionThread_;

    // ─── Constructor ─────────────────────────────────────────────────────────

    public BluetoothWirelessManager(Context context, Listener listener, HotspotConfig hotspot) {
        context_  = context;
        listener_ = listener;
        hotspot_  = hotspot;
    }

    // ─── Public API ──────────────────────────────────────────────────────────

    public void startListening() {
        stop();
        Thread t = new Thread(this::listenLoop, "BtWireless-Listen");
        listenThread_ = t;
        t.start();
    }

    public void stop() {
        closeCurrentClient();
        Thread lt = listenThread_;
        if (lt != null) { lt.interrupt(); listenThread_ = null; }
        closeServerSocket();
    }

    /**
     * Interrupt the current RFCOMM session thread, causing
     * waitForRfcommClose to exit and fire onDeviceDisconnected.
     * Called by WirelessMonitorService when the TCP session ends
     * (bidirectional lifecycle: TCP close -> RFCOMM close).
     */
    public void closeCurrentClient() {
        Thread st = sessionThread_;
        if (st != null) {
            st.interrupt();
        }
    }

    // ─── RFCOMM server loop ───────────────────────────────────────────────────

    private void listenLoop() {
        BluetoothAdapter bt = BluetoothAdapter.getDefaultAdapter();
        if (bt == null || !bt.isEnabled()) {
            Log.e(TAG, "Bluetooth not available");
            return;
        }

        try {
            serverSocket_ = bt.listenUsingRfcommWithServiceRecord(
                "AndroidAutoWireless", AAW_UUID);
            Log.i(TAG, "RFCOMM server listening on AAW UUID");
        } catch (IOException e) {
            Log.e(TAG, "RFCOMM listen failed: " + e.getMessage());
            return;
        }

        while (!Thread.currentThread().isInterrupted()) {
            Log.i(TAG, "RFCOMM waiting for connection...");
            BluetoothSocket socket;
            try {
                socket = serverSocket_.accept();
            } catch (IOException e) {
                if (!Thread.currentThread().isInterrupted()) {
                    Log.e(TAG, "RFCOMM accept error: " + e.getMessage());
                }
                break;
            }

            // Refuse a second connection while a session is alive.
            Thread st = sessionThread_;
            if (st != null && st.isAlive()) {
                String addr = socket.getRemoteDevice().getAddress();
                Log.w(TAG, "Second RFCOMM ignored — session thread alive (incoming=" + addr + ")");
                try { socket.close(); } catch (IOException ignored) {}
                continue;
            }

            String deviceId = socket.getRemoteDevice().getAddress();
            String name     = socket.getRemoteDevice().getName();
            if (name == null) name = deviceId;
            Log.i(TAG, "RFCOMM accepted from " + deviceId + " (" + name + ")");

            listener_.onDeviceConnecting(deviceId, name);

            // Socket ownership transfers to sessionThread_.
            final String devName = name;
            Thread t = new Thread(() -> runSession(socket, deviceId, devName),
                "BtWireless-" + deviceId);
            sessionThread_ = t;
            t.start();

            // Block until the session ends before accepting a new one.
            try {
                t.join();
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                break;
            }
        }

        closeServerSocket();
        Log.i(TAG, "RFCOMM server stopped");
    }

    // ─── Session (handshake + keep-alive) ─────────────────────────────────────

    private void runSession(BluetoothSocket socket, String deviceId, String deviceName) {
        String disconnectReason = "unknown";
        boolean handshakeOk = false;
        try {
            InputStream  in  = socket.getInputStream();
            OutputStream out = socket.getOutputStream();

            // 1. VERSION_REQUEST
            sendMessage(out, MSG_WIFI_VERSION_REQUEST, new byte[0]);
            Log.i(TAG, "Sent VERSION_REQUEST");

            // 2. START_REQUEST
            sendMessage(out, MSG_WIFI_START_REQUEST,
                encodeStartRequest(hotspot_.ip, hotspot_.port));
            Log.i(TAG, "Sent START_REQUEST: " + hotspot_.ip + ":" + hotspot_.port);

            // 3. Receive messages until START_RESPONSE
            int[] msgId = {0};
            while (true) {
                byte[] payload = readMessage(in, msgId);
                Log.i(TAG, "Received msgId=" + msgId[0] + " len=" + payload.length);

                switch (msgId[0]) {
                    case MSG_WIFI_INFO_REQUEST:
                        sendMessage(out, MSG_WIFI_INFO_RESPONSE,
                            encodeInfoResponse(hotspot_.ssid, hotspot_.password,
                                hotspot_.bssid, SECURITY_WPA2_PERSONAL, AP_TYPE_STATIC));
                        Log.i(TAG, "Sent INFO_RESPONSE: ssid=" + hotspot_.ssid);
                        break;

                    case MSG_WIFI_VERSION_RESPONSE:
                        Log.i(TAG, "Received VERSION_RESPONSE");
                        break;

                    case MSG_WIFI_CONNECTION_STATUS:
                        long status = parseProtoVarint(payload, 1);
                        Log.i(TAG, "Received CONNECTION_STATUS: status=" + status);
                        break;

                    case MSG_WIFI_START_RESPONSE:
                        long startStatus = parseProtoVarint(payload, 3);
                        Log.i(TAG, "Received START_RESPONSE: status=" + startStatus);
                        if (startStatus != 0) {
                            throw new IOException("START_RESPONSE status=" + startStatus);
                        }
                        handshakeOk = true;
                        listener_.onDeviceReady(deviceId, deviceName);
                        disconnectReason = waitForRfcommClose(in, deviceId);
                        return;

                    default:
                        Log.w(TAG, "Unexpected msgId=" + msgId[0] + ", ignoring");
                        break;
                }
            }

        } catch (Exception e) {
            disconnectReason = "failed: " + (e.getMessage() != null ? e.getMessage() : "unknown");
            Log.e(TAG, "Session error for " + deviceId + ": " + disconnectReason);
            if (!handshakeOk) {
                listener_.onConnectionFailed(deviceId, disconnectReason);
            }
        } finally {
            try { socket.close(); } catch (IOException ignored) {}
            sessionThread_ = null;
            // Always notify disconnect so the upstream tears down the AAP
            // session (TCP transport) as well.
            try {
                listener_.onDeviceDisconnected(deviceId, disconnectReason);
            } catch (Throwable t) {
                Log.w(TAG, "onDeviceDisconnected threw: " + t);
            }
            Log.i(TAG, "Session ended for " + deviceId + " (" + disconnectReason + ")");
        }
    }

    private String waitForRfcommClose(InputStream in, String deviceId) {
        Log.i(TAG, "RFCOMM keep-alive entered for " + deviceId);
        byte[] buf = new byte[256];
        try {
            while (!Thread.currentThread().isInterrupted()) {
                int n = in.read(buf);
                if (n < 0) {
                    Log.i(TAG, "RFCOMM peer closed for " + deviceId);
                    return "peer closed";
                }
                Log.i(TAG, "RFCOMM control bytes (" + n + ") from " + deviceId + " — discarded");
            }
            return "interrupted";
        } catch (IOException e) {
            Log.i(TAG, "RFCOMM read terminated for " + deviceId + ": " + e.getMessage());
            return "io error: " + e.getMessage();
        }
    }

    // ─── Packet I/O ──────────────────────────────────────────────────────────

    private static void sendMessage(OutputStream out, int msgId, byte[] payload) throws IOException {
        int len = payload.length;
        byte[] header = { (byte)(len >> 8), (byte) len, (byte)(msgId >> 8), (byte) msgId };
        out.write(header);
        if (len > 0) out.write(payload);
        out.flush();
    }

    private static byte[] readMessage(InputStream in, int[] outMsgId) throws IOException {
        byte[] header = readExact(in, 4);
        int len    = ((header[0] & 0xFF) << 8) | (header[1] & 0xFF);
        outMsgId[0] = ((header[2] & 0xFF) << 8) | (header[3] & 0xFF);
        return readExact(in, len);
    }

    private static byte[] readExact(InputStream in, int n) throws IOException {
        byte[] buf = new byte[n]; int offset = 0;
        while (offset < n) {
            int read = in.read(buf, offset, n - offset);
            if (read < 0) throw new IOException("Stream closed while reading");
            offset += read;
        }
        return buf;
    }

    // ─── Protobuf encoding ────────────────────────────────────────────────────

    private static byte[] encodeStartRequest(String ip, int port) {
        ByteArrayOutputStream buf = new ByteArrayOutputStream();
        writeProtoString(buf, 1, ip);
        writeProtoVarint(buf, 2, port);
        return buf.toByteArray();
    }

    private static byte[] encodeInfoResponse(String ssid, String password, String bssid,
                                              int secMode, int apType) {
        ByteArrayOutputStream buf = new ByteArrayOutputStream();
        writeProtoString(buf, 1, ssid);
        writeProtoString(buf, 2, password);
        writeProtoString(buf, 3, bssid);
        writeProtoVarint(buf, 4, secMode);
        writeProtoVarint(buf, 5, apType);
        return buf.toByteArray();
    }

    private static void writeProtoString(ByteArrayOutputStream buf, int fieldNum, String value) {
        byte[] b = value.getBytes(StandardCharsets.UTF_8);
        writeVarInt(buf, (fieldNum << 3) | 2);
        writeVarInt(buf, b.length);
        buf.write(b, 0, b.length);
    }

    private static void writeProtoVarint(ByteArrayOutputStream buf, int fieldNum, long value) {
        writeVarInt(buf, (fieldNum << 3) | 0);
        writeVarInt(buf, value);
    }

    private static void writeVarInt(ByteArrayOutputStream buf, long value) {
        while ((value & ~0x7FL) != 0) {
            buf.write((int)((value & 0x7F) | 0x80));
            value >>>= 7;
        }
        buf.write((int)(value & 0x7F));
    }

    // ─── Protobuf decoding ────────────────────────────────────────────────────

    private static long parseProtoVarint(byte[] data, int fieldNumber) {
        int[] i = {0};
        while (i[0] < data.length) {
            long tag = readVarint(data, i);
            int fn = (int)(tag >>> 3), wt = (int)(tag & 0x7);
            if (wt == 0) { long val = readVarint(data, i); if (fn == fieldNumber) return val; }
            else if (wt == 2) { int len = (int) readVarint(data, i); i[0] += len; }
            else break;
        }
        return 0;
    }

    private static long readVarint(byte[] data, int[] offset) {
        long value = 0; int shift = 0;
        while (offset[0] < data.length) {
            int b = data[offset[0]++] & 0xFF;
            value |= (long)(b & 0x7F) << shift;
            if ((b & 0x80) == 0) break;
            shift += 7;
        }
        return value;
    }

    // ─── Helpers ─────────────────────────────────────────────────────────────

    private void closeServerSocket() {
        BluetoothServerSocket s = serverSocket_;
        serverSocket_ = null;
        if (s != null) { try { s.close(); } catch (IOException ignored) {} }
    }
}
