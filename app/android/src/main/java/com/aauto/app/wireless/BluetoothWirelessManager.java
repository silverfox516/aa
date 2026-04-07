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
 * Handles the Wireless Android Auto (AAW) Bluetooth RFCOMM handshake.
 *
 * HU acts as RFCOMM server: listens on the AAW UUID and waits for the phone
 * to initiate the connection.
 *
 * Packet format: [2-byte payload_length][2-byte msgId][protobuf payload]
 *
 * Message IDs:
 *   1 = WIFI_START_REQUEST      (HU → Phone)
 *   2 = WIFI_INFO_REQUEST       (Phone → HU)
 *   3 = WIFI_INFO_RESPONSE      (HU → Phone)
 *   4 = WIFI_VERSION_REQUEST    (HU → Phone)
 *   5 = WIFI_VERSION_RESPONSE   (Phone → HU)
 *   6 = WIFI_CONNECTION_STATUS  (Phone → HU)
 *   7 = WIFI_START_RESPONSE     (Phone → HU)
 *
 * Protocol sequence:
 *   HU   → Phone : VERSION_REQUEST (4)  [empty]
 *   HU   → Phone : START_REQUEST   (1)  [ip_address, port=5277]
 *   Phone→ HU    : INFO_REQUEST    (2)  [empty]
 *   HU   → Phone : INFO_RESPONSE   (3)  [ssid, password, bssid, security_mode, ap_type]
 *   Phone→ HU    : VERSION_RESPONSE(5)
 *   Phone→ HU    : CONNECTION_STATUS(6)
 *   Phone→ HU    : START_RESPONSE  (7)  → handshake complete
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

    // WifiSecurityMode: WPA2_PERSONAL = 5
    private static final int SECURITY_WPA2_PERSONAL = 5;
    // AccessPointType: STATIC = 0
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
        /** RFCOMM accepted — handshake starting. */
        void onDeviceConnecting(String deviceId, String deviceName);
        /** RFCOMM handshake succeeded — phone will connect to HU TCP server. */
        void onDeviceReady(String deviceId, String deviceName);
        void onConnectionFailed(String deviceId, String reason);
    }

    // ─── Fields ──────────────────────────────────────────────────────────────

    private final Context       context_;
    private final Listener      listener_;
    private final HotspotConfig hotspot_;

    private volatile BluetoothServerSocket serverSocket_;
    private volatile BluetoothSocket       clientSocket_;
    private volatile Thread                listenThread_;
    private volatile Thread                handshakeThread_;

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
        Thread ht = handshakeThread_;
        if (ht != null) { ht.interrupt(); handshakeThread_ = null; }
        Thread lt = listenThread_;
        if (lt != null) { lt.interrupt(); listenThread_ = null; }
        closeServerSocket();
        closeClientSocket();
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
            BluetoothSocket socket;
            try {
                socket = serverSocket_.accept();
            } catch (IOException e) {
                if (!Thread.currentThread().isInterrupted()) {
                    Log.e(TAG, "RFCOMM accept error: " + e.getMessage());
                }
                break;
            }

            String deviceId = socket.getRemoteDevice().getAddress();
            String name     = socket.getRemoteDevice().getName();
            if (name == null) name = deviceId;
            Log.i(TAG, "RFCOMM accepted from " + deviceId + " (" + name + ")");
            closeClientSocket();
            clientSocket_ = socket;
            listener_.onDeviceConnecting(deviceId, name);

            final String devName = name;
            Thread t = new Thread(() -> runHandshake(socket, deviceId, devName),
                "BtWireless-" + deviceId);
            handshakeThread_ = t;
            t.start();

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

    // ─── Handshake ────────────────────────────────────────────────────────────

    private void runHandshake(BluetoothSocket socket, String deviceId, String deviceName) {
        try {
            InputStream  in  = socket.getInputStream();
            OutputStream out = socket.getOutputStream();

            // 1. VERSION_REQUEST
            sendMessage(out, MSG_WIFI_VERSION_REQUEST, new byte[0]);
            Log.i(TAG, "Sent VERSION_REQUEST");

            // 2. START_REQUEST [ip, port]
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
                        Log.i(TAG, "Received INFO_REQUEST, sending INFO_RESPONSE");
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
                        closeClientSocket();
                        listener_.onDeviceReady(deviceId, deviceName);
                        return;

                    default:
                        Log.w(TAG, "Unexpected msgId=" + msgId[0] + ", ignoring");
                        break;
                }
            }

        } catch (Exception e) {
            Log.e(TAG, "Handshake failed for " + deviceId + ": " + e.getMessage(), e);
            closeClientSocket();
            listener_.onConnectionFailed(deviceId, e.getMessage() != null ? e.getMessage() : "unknown");
        }
    }

    // ─── Packet I/O ──────────────────────────────────────────────────────────

    /** [2-byte payload_len][2-byte msgId][payload] */
    private static void sendMessage(OutputStream out, int msgId, byte[] payload)
            throws IOException {
        int len = payload.length;
        byte[] header = {
            (byte)(len   >> 8), (byte) len,
            (byte)(msgId >> 8), (byte) msgId
        };
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
        byte[] buf    = new byte[n];
        int    offset = 0;
        while (offset < n) {
            int read = in.read(buf, offset, n - offset);
            if (read < 0) throw new IOException("Stream closed while reading");
            offset += read;
        }
        return buf;
    }

    // ─── Protobuf encoding ────────────────────────────────────────────────────

    /** WifiStartRequest { string ip_address = 1; uint32 port = 2; } */
    private static byte[] encodeStartRequest(String ip, int port) {
        ByteArrayOutputStream buf = new ByteArrayOutputStream();
        writeProtoString(buf, 1, ip);
        writeProtoVarint(buf, 2, port);
        return buf.toByteArray();
    }

    /** WifiInfoResponse { string ssid=1; string password=2; string bssid=3;
     *                     WifiSecurityMode security_mode=4; AccessPointType access_point_type=5; } */
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
        writeVarInt(buf, (fieldNum << 3) | 2);  // wire type 2 = length-delimited
        writeVarInt(buf, b.length);
        buf.write(b, 0, b.length);
    }

    private static void writeProtoVarint(ByteArrayOutputStream buf, int fieldNum, long value) {
        writeVarInt(buf, (fieldNum << 3) | 0);  // wire type 0 = varint
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
            int  fn  = (int)(tag >>> 3);
            int  wt  = (int)(tag & 0x7);
            if (wt == 0) {
                long val = readVarint(data, i);
                if (fn == fieldNumber) return val;
            } else if (wt == 2) {
                int len = (int) readVarint(data, i);
                i[0] += len;
            } else {
                break;
            }
        }
        return 0;
    }

    private static long readVarint(byte[] data, int[] offset) {
        long value = 0;
        int  shift = 0;
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
        if (s != null) {
            try { s.close(); } catch (IOException ignored) {}
        }
    }

    private void closeClientSocket() {
        BluetoothSocket s = clientSocket_;
        clientSocket_ = null;
        if (s != null) {
            try { s.close(); } catch (IOException ignored) {}
        }
    }
}
