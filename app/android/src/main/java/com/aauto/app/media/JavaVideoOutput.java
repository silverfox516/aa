package com.aauto.app.media;

import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.util.Log;
import android.view.Surface;

import java.io.IOException;
import java.nio.ByteBuffer;

/**
 * H.264 video decoder backed by Java MediaCodec.
 * Called from native via JNI (JavaVideoOutput.cpp).
 *
 * The native layer calls open(), pushData(), and close()
 * through JNI function pointers registered on construction.
 */
public class JavaVideoOutput {

    private static final String TAG  = "AA.IMPL.JavaVideoOutput";
    private static final String MIME = "video/avc";

    private MediaCodec codec_   = null;
    private Surface    surface_ = null;
    private boolean    isOpen_  = false;

    public JavaVideoOutput() {}

    public void setSurface(Surface surface) {
        surface_ = surface;
    }

    /** Called from native: JNI_JavaVideoOutput_open */
    public boolean open(int width, int height) {
        if (isOpen_) {
            Log.w(TAG, "Already open");
            return true;
        }
        if (surface_ == null) {
            Log.e(TAG, "No surface set");
            return false;
        }
        try {
            codec_ = MediaCodec.createDecoderByType(MIME);

            MediaFormat format = MediaFormat.createVideoFormat(MIME, width, height);
            // CSD-0 (SPS) and CSD-1 (PPS) are embedded in the stream for Android Auto
            format.setInteger(MediaFormat.KEY_COLOR_FORMAT,
                MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);

            codec_.configure(format, surface_, null, 0);
            codec_.start();
            isOpen_ = true;
            Log.i(TAG, "Opened " + width + "x" + height);
            return true;
        } catch (IOException e) {
            Log.e(TAG, "Failed to create MediaCodec: " + e.getMessage());
            return false;
        }
    }

    /** Called from native: JNI_JavaVideoOutput_pushData */
    public void pushData(byte[] data) {
        if (!isOpen_ || codec_ == null || data == null) return;

        int inputIndex = codec_.dequeueInputBuffer(10000 /* 10ms */);
        if (inputIndex < 0) return;

        ByteBuffer inputBuf = codec_.getInputBuffer(inputIndex);
        if (inputBuf == null) return;

        inputBuf.clear();
        int copyLen = Math.min(data.length, inputBuf.capacity());
        inputBuf.put(data, 0, copyLen);
        codec_.queueInputBuffer(inputIndex, 0, copyLen, 0, 0);

        // Release output buffer to surface
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        int outputIndex = codec_.dequeueOutputBuffer(info, 0);
        while (outputIndex >= 0) {
            codec_.releaseOutputBuffer(outputIndex, /*render=*/true);
            outputIndex = codec_.dequeueOutputBuffer(info, 0);
        }
    }

    /** Called from native: JNI_JavaVideoOutput_close */
    public void close() {
        if (!isOpen_) return;
        isOpen_ = false;
        if (codec_ != null) {
            codec_.stop();
            codec_.release();
            codec_ = null;
        }
        Log.i(TAG, "Closed");
    }
}
