package com.aauto.app.media;

import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.util.Log;

/**
 * PCM audio output backed by Java AudioTrack.
 * Called from native via JNI (JavaAudioOutput.cpp).
 */
public class JavaAudioOutput {

    private static final String TAG = "AA.IMPL.JavaAudioOutput";

    private AudioTrack track_  = null;
    private boolean    isOpen_ = false;

    public JavaAudioOutput() {}

    /** Called from native: JNI_JavaAudioOutput_open */
    public boolean open(int sampleRate, int channels, int bits) {
        if (isOpen_) {
            Log.w(TAG, "Already open");
            return true;
        }

        int channelConfig = (channels == 2)
            ? AudioFormat.CHANNEL_OUT_STEREO
            : AudioFormat.CHANNEL_OUT_MONO;

        int encoding = (bits == 16)
            ? AudioFormat.ENCODING_PCM_16BIT
            : AudioFormat.ENCODING_PCM_8BIT;

        int minBufSize = AudioTrack.getMinBufferSize(sampleRate, channelConfig, encoding);
        if (minBufSize <= 0) {
            Log.e(TAG, "Invalid AudioTrack config: rate=" + sampleRate
                    + " ch=" + channels + " bits=" + bits);
            return false;
        }

        track_ = new AudioTrack(
            AudioManager.STREAM_MUSIC,
            sampleRate,
            channelConfig,
            encoding,
            minBufSize * 4,
            AudioTrack.MODE_STREAM);

        if (track_.getState() != AudioTrack.STATE_INITIALIZED) {
            Log.e(TAG, "AudioTrack initialization failed");
            track_ = null;
            return false;
        }

        isOpen_ = true;
        Log.i(TAG, "Opened " + sampleRate + "Hz " + channels + "ch " + bits + "bit");
        return true;
    }

    /** Called from native: JNI_JavaAudioOutput_start */
    public void start() {
        if (!isOpen_ || track_ == null) return;
        track_.play();
        Log.i(TAG, "Playback started");
    }

    /** Called from native: JNI_JavaAudioOutput_pushData */
    public void pushData(byte[] data) {
        if (!isOpen_ || track_ == null || data == null) return;
        track_.write(data, 0, data.length);
    }

    /** Called from native: JNI_JavaAudioOutput_close */
    public void close() {
        if (!isOpen_) return;
        isOpen_ = false;
        if (track_ != null) {
            track_.stop();
            track_.release();
            track_ = null;
        }
        Log.i(TAG, "Closed");
    }
}
