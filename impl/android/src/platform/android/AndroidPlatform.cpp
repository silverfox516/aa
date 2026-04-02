#define LOG_TAG "AA.AndroidPlatform"

#include "aauto/platform/android/AndroidPlatform.hpp"

#include <android/native_window_jni.h>

#ifdef AAUTO_MEDIA_NATIVE
#include "aauto/platform/android/NativeVideoOutput.hpp"
#include "aauto/platform/android/NativeAudioOutput.hpp"
#else
#include "aauto/platform/android/JavaVideoOutput.hpp"
#include "aauto/platform/android/JavaAudioOutput.hpp"
#endif

#include "aauto/utils/Logger.hpp"

namespace aauto {
namespace platform {
namespace android {

// ─── TouchCapturingProxy ──────────────────────────────────────────────────────
//
// Wraps a real IVideoOutput and intercepts SetTouchCallback so that
// AndroidPlatform can dispatch touch events from the JNI layer without
// modifying the IVideoOutput interface.
//
// When InputService calls SetTouchCallback(cb):
//   1. The callback is copied into the platform's touch_callback_ storage.
//   2. The same callback is forwarded to the wrapped output (in case it needs it).

class TouchCapturingProxy : public IVideoOutput {
public:
    TouchCapturingProxy(std::shared_ptr<IVideoOutput> wrapped,
                        std::mutex& cb_mutex,
                        TouchCallback& cb_storage)
        : wrapped_(std::move(wrapped))
        , cb_mutex_(cb_mutex)
        , cb_storage_(cb_storage) {}

    void Open(int width, int height) override   { wrapped_->Open(width, height); }
    void Close() override                        { wrapped_->Close(); }
    void PushVideoData(const std::vector<uint8_t>& data, bool is_codec_config) override {
        wrapped_->PushVideoData(data, is_codec_config);
    }
    void SetTouchCallback(TouchCallback cb) override {
        {
            std::lock_guard<std::mutex> lock(cb_mutex_);
            cb_storage_ = cb;  // capture for JNI dispatch
        }
        wrapped_->SetTouchCallback(std::move(cb));
    }

    IVideoOutput* Wrapped() const { return wrapped_.get(); }

private:
    std::shared_ptr<IVideoOutput> wrapped_;
    std::mutex&    cb_mutex_;
    TouchCallback& cb_storage_;
};

// ─── AndroidPlatform ──────────────────────────────────────────────────────────

AndroidPlatform::AndroidPlatform(JavaVM* jvm, jobject surface)
    : jvm_(jvm) {
    if (jvm_ && surface) {
        JNIEnv* env = nullptr;
        jvm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (env) surface_ref_ = env->NewGlobalRef(surface);
    }
}

AndroidPlatform::~AndroidPlatform() {
    Stop();
    if (surface_ref_ && jvm_) {
        JNIEnv* env = nullptr;
        jvm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (env) env->DeleteGlobalRef(surface_ref_);
        surface_ref_ = nullptr;
    }
}

void AndroidPlatform::SetSurface(ANativeWindow* window) {
#ifdef AAUTO_MEDIA_NATIVE
    // native_video_ is the underlying NativeVideoOutput (not the proxy)
    if (native_video_) native_video_->SetSurface(window);
#else
    (void)window;
    AA_LOG_W() << "SetSurface: Java backend surface is set at construction time";
#endif
}

void AndroidPlatform::SetViewSize(int width, int height) {
    if (width > 0 && height > 0) {
        view_width_  = width;
        view_height_ = height;
        AA_LOG_I() << "View size updated: " << view_width_ << "x" << view_height_;
    }
}

void AndroidPlatform::DispatchTouchEvent(int pointer_id, float x, float y, int action) {
    // Scale from SurfaceView pixel coordinates to the AA display coordinate space.
    // The phone expects coordinates in [0, display_width) x [0, display_height)
    // as advertised in InputService::FillServiceDefinition.
    int mapped_x = static_cast<int>(x * display_width_  / view_width_);
    int mapped_y = static_cast<int>(y * display_height_ / view_height_);

    std::lock_guard<std::mutex> lock(touch_cb_mutex_);
    if (touch_callback_) {
        touch_callback_(TouchEvent{mapped_x, mapped_y, pointer_id, action});
    }
}

bool AndroidPlatform::Initialize() {
    std::shared_ptr<IVideoOutput> raw_video;

#ifdef AAUTO_MEDIA_NATIVE
    AA_LOG_I() << "Initializing with native backend (NDK MediaCodec + OpenSL ES)";
    auto native = std::make_shared<NativeVideoOutput>();
    native_video_ = native;
    raw_video     = native;
    audio_output_ = std::make_shared<NativeAudioOutput>();
#else
    AA_LOG_I() << "Initializing with Java backend (Java MediaCodec + AudioTrack)";
    raw_video     = std::make_shared<JavaVideoOutput>(jvm_, surface_ref_);
    audio_output_ = std::make_shared<JavaAudioOutput>(jvm_);
#endif

    // Wrap the real video output in a proxy that captures SetTouchCallback calls
    video_output_ = std::make_shared<TouchCapturingProxy>(
        raw_video, touch_cb_mutex_, touch_callback_);

    return true;
}

std::shared_ptr<IVideoOutput> AndroidPlatform::GetVideoOutput() {
    return video_output_;
}

std::shared_ptr<IAudioOutput> AndroidPlatform::GetAudioOutput() {
    return audio_output_;
}

std::shared_ptr<IAudioOutput> AndroidPlatform::CreateAudioOutput() {
#ifdef AAUTO_MEDIA_NATIVE
    return std::make_shared<NativeAudioOutput>();
#else
    return std::make_shared<JavaAudioOutput>(jvm_);
#endif
}

void AndroidPlatform::Run() {
    std::unique_lock<std::mutex> lock(run_mutex_);
    run_cv_.wait(lock, [this] { return stop_requested_; });
}

void AndroidPlatform::Stop() {
    {
        std::lock_guard<std::mutex> lock(run_mutex_);
        stop_requested_ = true;
    }
    run_cv_.notify_all();
}

}  // namespace android
}  // namespace platform
}  // namespace aauto
