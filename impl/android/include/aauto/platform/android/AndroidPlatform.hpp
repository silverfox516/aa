#pragma once

#include <condition_variable>
#include <jni.h>
#include <memory>
#include <mutex>

#include <android/native_window.h>

#include "aauto/platform/IPlatform.hpp"

namespace aauto {
namespace platform {
namespace android {

/**
 * Android platform implementation.
 *
 * Selects the A/V backend at compile time:
 *   AAUTO_MEDIA_NATIVE  -> NativeVideoOutput (NDK MediaCodec) + NativeAudioOutput (OpenSL ES)
 *   AAUTO_MEDIA_JAVA    -> JavaVideoOutput   (Java MediaCodec) + JavaAudioOutput  (AudioTrack)
 *
 * Run() blocks until Stop() is called (Android is event-driven; the engine
 * runs its own service threads, so Run() just keeps the engine alive).
 */
class AndroidPlatform : public IPlatform {
public:
    AndroidPlatform(JavaVM* jvm, jobject surface);
    ~AndroidPlatform() override;

    /**
     * Update the rendering surface. Must be called from the JNI layer
     * whenever the SurfaceView surface is created or changed.
     */
    void SetSurface(ANativeWindow* window);

    /**
     * Update the view dimensions used to scale touch coordinates.
     * Must be called whenever the SurfaceView size changes (surfaceChanged).
     */
    void SetViewSize(int width, int height);

    /**
     * Deliver a touch event from the Java UI to the registered InputService callback.
     * Raw SurfaceView pixel coordinates are scaled to the AA display resolution.
     * Called from JNI on touch events from SurfaceView.
     */
    void DispatchTouchEvent(int pointer_id, float x, float y, int action);

    // IPlatform
    bool Initialize() override;
    std::shared_ptr<IVideoOutput> GetVideoOutput() override;
    std::shared_ptr<IAudioOutput> GetAudioOutput() override;
    std::shared_ptr<IAudioOutput> CreateAudioOutput() override;
    void Run() override;
    void Stop() override;

private:
    JavaVM*   jvm_;
    jobject   surface_ref_ = nullptr;  // GlobalRef

    std::shared_ptr<IVideoOutput> video_output_;   // TouchCapturingProxy wrapping real output
    std::shared_ptr<IAudioOutput> audio_output_;

    // Direct reference to the underlying NativeVideoOutput for SetSurface()
    // (only populated when AAUTO_MEDIA_NATIVE is defined)
    std::shared_ptr<class NativeVideoOutput> native_video_;

    // Touch callback registered by InputService via IVideoOutput::SetTouchCallback.
    // We proxy it here so JNI can dispatch without going through IVideoOutput internals.
    TouchCallback touch_callback_;
    std::mutex    touch_cb_mutex_;

    // SurfaceView dimensions — updated via SetViewSize() on each surfaceChanged.
    // Used to scale raw pixel coordinates to the AA display resolution (display_width x display_height).
    // AA display resolution matches HeadunitConfig defaults (1280x720).
    int view_width_    = 1280;
    int view_height_   = 720;
    int display_width_ = 1280;
    int display_height_ = 720;

    std::mutex              run_mutex_;
    std::condition_variable run_cv_;
    bool                    stop_requested_ = false;
};

}  // namespace android
}  // namespace platform
}  // namespace aauto
