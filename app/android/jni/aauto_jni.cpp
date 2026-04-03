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
#include "aauto/service/ControlService.hpp"
#include "aauto/service/VideoService.hpp"
#include "aauto/transport/android/AndroidUsbTransport.hpp"
// TcpTransport included in Stage 8 when the class is implemented.
// #include "aauto/transport/android/TcpTransport.hpp"
#include "aauto/utils/Logger.hpp"

using namespace aauto;

// ─── Module-level singletons ──────────────────────────────────────────────────

namespace {

struct EngineContext {
    std::unique_ptr<core::DeviceManager>                    device_manager;
    std::shared_ptr<platform::android::AndroidPlatform>     platform;
    std::unique_ptr<core::AAutoEngine>                      engine;
};

std::mutex     g_mutex;
EngineContext* g_ctx = nullptr;
JavaVM*        g_jvm = nullptr;

/** Helper: get VideoService for a session, or nullptr. */
service::VideoService* GetVideoService(const std::string& device_id) {
    if (!g_ctx) return nullptr;
    auto session = g_ctx->engine->GetSession(device_id);
    if (!session) return nullptr;
    auto svc = session->GetService(service::ServiceType::VIDEO);
    return dynamic_cast<service::VideoService*>(svc.get());
}

/** Helper: get ControlService for a session, or nullptr. */
service::ControlService* GetControlService(const std::string& device_id) {
    if (!g_ctx) return nullptr;
    auto session = g_ctx->engine->GetSession(device_id);
    if (!session) return nullptr;
    auto svc = session->GetService(service::ServiceType::CONTROL);
    return dynamic_cast<service::ControlService*>(svc.get());
}

}  // namespace

// Called by the JVM when the library is loaded.
JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    g_jvm = vm;
    return JNI_VERSION_1_6;
}

// ─── JNI implementations ──────────────────────────────────────────────────────

extern "C" {

/**
 * Called once from AaSessionService.onCreate().
 * Creates the engine and platform. No surface or transport yet.
 */
JNIEXPORT void JNICALL
Java_com_aauto_app_core_AaSessionService_nativeInit(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_ctx) {
        AA_LOG_W() << "nativeInit: already initialized";
        return;
    }

    auto* ctx = new EngineContext();

    ctx->platform = std::make_shared<platform::android::AndroidPlatform>(g_jvm, nullptr);
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
 * Called from AaSessionService.onDestroy().
 */
JNIEXPORT void JNICALL
Java_com_aauto_app_core_AaSessionService_nativeDestroy(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(g_mutex);
    delete g_ctx;
    g_ctx = nullptr;
    AA_LOG_I() << "Engine destroyed";
}

/**
 * Called when a surface becomes available (AaDisplayActivity.surfaceCreated/Changed).
 * Sets the rendering surface and sends VideoFocusGain to start phone video streaming.
 */
JNIEXPORT void JNICALL
Java_com_aauto_app_core_AaSessionService_nativeSurfaceReady(JNIEnv* env, jobject /*thiz*/,
                                                              jstring deviceId,
                                                              jobject surface) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ctx) return;

    const char* id_cstr = env->GetStringUTFChars(deviceId, nullptr);
    std::string id(id_cstr ? id_cstr : "");
    if (id_cstr) env->ReleaseStringUTFChars(deviceId, id_cstr);

    ANativeWindow* window = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    g_ctx->platform->SetSurface(window);
    if (window) ANativeWindow_release(window);

    auto* video_svc = GetVideoService(id);
    if (video_svc) {
        video_svc->SendVideoFocusGain();
    } else {
        AA_LOG_W() << "nativeSurfaceReady: no VideoService for id=" << id;
    }
    auto* ctrl_svc = GetControlService(id);
    if (ctrl_svc) ctrl_svc->SendAudioFocusGain();
    AA_LOG_I() << "Surface ready for device=" << id;
}

/**
 * Called when the surface is destroyed (AaDisplayActivity paused or finished).
 * Clears the rendering surface and sends VideoFocusLoss to stop phone video streaming.
 * The session remains alive.
 */
JNIEXPORT void JNICALL
Java_com_aauto_app_core_AaSessionService_nativeSurfaceDestroyed(JNIEnv* env, jobject /*thiz*/,
                                                                  jstring deviceId) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ctx) return;

    const char* id_cstr = env->GetStringUTFChars(deviceId, nullptr);
    std::string id(id_cstr ? id_cstr : "");
    if (id_cstr) env->ReleaseStringUTFChars(deviceId, id_cstr);

    auto* ctrl_svc = GetControlService(id);
    if (ctrl_svc) ctrl_svc->SendAudioFocusLoss();
    auto* video_svc = GetVideoService(id);
    if (video_svc) video_svc->SendVideoFocusLoss();
    g_ctx->platform->SetSurface(nullptr);
    AA_LOG_I() << "Surface destroyed for device=" << id;
}

JNIEXPORT void JNICALL
Java_com_aauto_app_core_AaSessionService_nativeSetViewSize(JNIEnv* /*env*/, jobject /*thiz*/,
                                                            jint width, jint height) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ctx) return;
    g_ctx->platform->SetViewSize(static_cast<int>(width), static_cast<int>(height));
}

/**
 * Called from AaSessionService when a USB AOA device is ready.
 */
JNIEXPORT void JNICALL
Java_com_aauto_app_core_AaSessionService_nativeOnUsbDeviceReady(JNIEnv* env, jobject /*thiz*/,
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
 * Called when the USB device is detached.
 */
JNIEXPORT void JNICALL
Java_com_aauto_app_core_AaSessionService_nativeOnUsbDeviceDetached(JNIEnv* env, jobject /*thiz*/,
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
 * Called from AaSessionService when a wireless TCP transport is ready.
 * Full implementation added in Stage 8 (TcpTransport).
 */
JNIEXPORT void JNICALL
Java_com_aauto_app_core_AaSessionService_nativeOnWirelessDeviceReady(JNIEnv* env, jobject /*thiz*/,
                                                                       jstring ip,
                                                                       jint port,
                                                                       jstring deviceId) {
    const char* ip_cstr = env->GetStringUTFChars(ip, nullptr);
    const char* id_cstr = env->GetStringUTFChars(deviceId, nullptr);
    AA_LOG_W() << "nativeOnWirelessDeviceReady: TcpTransport not yet implemented"
               << " ip=" << (ip_cstr ? ip_cstr : "") << " port=" << port
               << " id=" << (id_cstr ? id_cstr : "");
    if (ip_cstr) env->ReleaseStringUTFChars(ip, ip_cstr);
    if (id_cstr) env->ReleaseStringUTFChars(deviceId, id_cstr);
}

/**
 * Called when the wireless device is detached.
 */
JNIEXPORT void JNICALL
Java_com_aauto_app_core_AaSessionService_nativeOnWirelessDeviceDetached(JNIEnv* env, jobject /*thiz*/,
                                                                          jstring deviceId) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ctx) return;

    const char* id_cstr = env->GetStringUTFChars(deviceId, nullptr);
    std::string id(id_cstr ? id_cstr : "wireless_device");
    if (id_cstr) env->ReleaseStringUTFChars(deviceId, id_cstr);

    AA_LOG_I() << "Wireless device detached: " << id;
    g_ctx->device_manager->NotifyDeviceDisconnected(id);
}

/**
 * Called from AaDisplayActivity touch events, routed via AaSessionService binder.
 */
JNIEXPORT void JNICALL
Java_com_aauto_app_core_AaSessionService_nativeDispatchTouchEvent(JNIEnv* /*env*/, jobject /*thiz*/,
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
