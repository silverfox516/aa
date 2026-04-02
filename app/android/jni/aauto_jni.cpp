#define LOG_TAG "AA.AAutoJNI"

#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>

#include <memory>
#include <mutex>
#include <string>

#include "aauto/core/AAutoEngine.hpp"
#include "aauto/core/DeviceManager.hpp"
#include "aauto/platform/android/AndroidPlatform.hpp"
#include "aauto/transport/android/AndroidUsbTransport.hpp"
#include "aauto/utils/Logger.hpp"

using namespace aauto;

// ─── Module-level singletons ──────────────────────────────────────────────────

namespace {

struct EngineContext {
    std::unique_ptr<core::DeviceManager>                    device_manager;
    std::shared_ptr<platform::android::AndroidPlatform>     platform;
    std::unique_ptr<core::AAutoEngine>                      engine;
};

std::mutex              g_mutex;
EngineContext*          g_ctx      = nullptr;
JavaVM*                 g_jvm      = nullptr;

}  // namespace

// Called by the JVM when the library is loaded
JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    g_jvm = vm;
    return JNI_VERSION_1_6;
}

// ─── JNI implementations ──────────────────────────────────────────────────────

extern "C" {

/**
 * Called once from Activity.onCreate().
 * Creates the engine and platform but does NOT start I/O yet.
 * surface: the jobject of the SurfaceView's Surface (may be null at this point).
 */
JNIEXPORT void JNICALL
Java_com_aauto_app_MainActivity_nativeInit(JNIEnv* env, jobject /*thiz*/,
                                           jobject surface) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_ctx) {
        AA_LOG_W() << "nativeInit: already initialized";
        return;
    }

    auto* ctx = new EngineContext();

    ctx->platform = std::make_shared<platform::android::AndroidPlatform>(g_jvm, surface);
    if (!ctx->platform->Initialize()) {
        AA_LOG_E() << "Platform initialization failed";
        delete ctx;
        return;
    }

    ctx->device_manager = std::make_unique<core::DeviceManager>();
    ctx->engine = std::make_unique<core::AAutoEngine>(
        *ctx->device_manager, ctx->platform);

    g_ctx = ctx;
    AA_LOG_I() << "Engine initialized";
}

/**
 * Called from Activity.onResume().
 * No-op here; USB connection triggers the actual session start via nativeOnUsbDeviceReady.
 */
JNIEXPORT void JNICALL
Java_com_aauto_app_MainActivity_nativeStart(JNIEnv* /*env*/, jobject /*thiz*/) {
    AA_LOG_I() << "nativeStart called";
}

/**
 * Called from Activity.onPause().
 * Stops the platform event loop; sessions are torn down by DeviceManager.
 */
JNIEXPORT void JNICALL
Java_com_aauto_app_MainActivity_nativeStop(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ctx) return;
    g_ctx->platform->Stop();
    AA_LOG_I() << "nativeStop called";
}

/**
 * Called from Activity.onDestroy().
 */
JNIEXPORT void JNICALL
Java_com_aauto_app_MainActivity_nativeDestroy(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(g_mutex);
    delete g_ctx;
    g_ctx = nullptr;
    AA_LOG_I() << "Engine destroyed";
}

/**
 * Called from SurfaceHolder.Callback.surfaceCreated/Changed.
 * Passes the ANativeWindow to the platform so the video decoder can render.
 */
JNIEXPORT void JNICALL
Java_com_aauto_app_MainActivity_nativeSetSurface(JNIEnv* env, jobject /*thiz*/,
                                                  jobject surface) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ctx) return;

    ANativeWindow* window = surface
        ? ANativeWindow_fromSurface(env, surface)
        : nullptr;

    g_ctx->platform->SetSurface(window);

    if (window) ANativeWindow_release(window);
    AA_LOG_I() << "Surface updated: " << window;
}

JNIEXPORT void JNICALL
Java_com_aauto_app_MainActivity_nativeSetViewSize(JNIEnv* /*env*/, jobject /*thiz*/,
                                                   jint width, jint height) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ctx) return;
    g_ctx->platform->SetViewSize(static_cast<int>(width), static_cast<int>(height));
}

/**
 * Called from UsbAccessoryManager when an AOA device is ready.
 * Creates an AndroidUsbTransport and notifies DeviceManager -> AAutoEngine.
 *
 * fd:       file descriptor from UsbDeviceConnection.getFileDescriptor()
 * deviceId: device path (e.g. "/dev/bus/usb/001/002")
 */
JNIEXPORT void JNICALL
Java_com_aauto_app_MainActivity_nativeOnUsbDeviceReady(JNIEnv* env, jobject /*thiz*/,
                                                        jint fd,
                                                        jint ep_in,
                                                        jint ep_out,
                                                        jstring deviceId) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ctx) {
        AA_LOG_E() << "nativeOnUsbDeviceReady: engine not initialized";
        return;
    }

    const char* id_cstr = env->GetStringUTFChars(deviceId, nullptr);
    std::string id(id_cstr ? id_cstr : "usb_device");
    if (id_cstr) env->ReleaseStringUTFChars(deviceId, id_cstr);

    AA_LOG_I() << "USB device ready: id=" << id << " fd=" << fd
               << " ep_in=0x" << std::hex << ep_in << " ep_out=0x" << ep_out << std::dec;

    auto transport = std::make_shared<transport::AndroidUsbTransport>(
        static_cast<int>(fd), id,
        static_cast<int>(ep_in), static_cast<int>(ep_out));

    transport::DeviceInfo device_info{id, "Android Auto Phone", transport::TransportType::USB};
    g_ctx->device_manager->NotifyDeviceConnected(device_info, transport);
}

/**
 * Called from UsbAccessoryManager when the AOA device is detached.
 */
JNIEXPORT void JNICALL
Java_com_aauto_app_MainActivity_nativeOnUsbDeviceDetached(JNIEnv* env, jobject /*thiz*/,
                                                           jstring deviceId) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ctx) return;

    const char* id_cstr = env->GetStringUTFChars(deviceId, nullptr);
    std::string id(id_cstr ? id_cstr : "usb_device");
    if (id_cstr) env->ReleaseStringUTFChars(deviceId, id_cstr);

    AA_LOG_I() << "USB device detached: " << id;
    g_ctx->device_manager->NotifyDeviceDisconnected(id);
}

/**
 * Called from SurfaceView's touch event handler.
 * Delivers touch events to the engine's input service.
 */
JNIEXPORT void JNICALL
Java_com_aauto_app_MainActivity_nativeOnTouchEvent(JNIEnv* /*env*/, jobject /*thiz*/,
                                                    jint pointer_id,
                                                    jfloat x, jfloat y,
                                                    jint action) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ctx) return;
    g_ctx->platform->DispatchTouchEvent(
        static_cast<int>(pointer_id),
        static_cast<float>(x),
        static_cast<float>(y),
        static_cast<int>(action));
}

}  // extern "C"
