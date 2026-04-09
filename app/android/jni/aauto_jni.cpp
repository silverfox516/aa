#define LOG_TAG "AA.IMPL.JNI"

#include <jni.h>
#include <android/native_window_jni.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "aauto/core/AAutoEngine.hpp"
#include "aauto/output/MediaCodecVideoSink.hpp"
#include "aauto/output/OpenSLAudioSink.hpp"
#include "aauto/service/AudioService.hpp"
#include "aauto/service/BluetoothService.hpp"
#include "aauto/service/InputService.hpp"
#include "aauto/service/IService.hpp"
#include "aauto/service/MicrophoneService.hpp"
#include "aauto/service/SensorService.hpp"
#include "aauto/service/ServiceComposition.hpp"
#include "aauto/service/VideoService.hpp"
#include "aauto/session/Session.hpp"
#include "aauto/transport/android/AndroidUsbTransport.hpp"
#include "aauto/transport/android/TcpTransport.hpp"
#include "aauto/utils/Logger.hpp"

using namespace aauto;

// ─── Module-level state ───────────────────────────────────────────────────────

namespace {

constexpr int kAudioChannelCount = 3;  // 0=MEDIA, 1=GUIDANCE, 2=SYSTEM

struct SessionEntry {
    std::shared_ptr<session::Session>                            session;
    std::shared_ptr<output::android::MediaCodecVideoSink>        video_sink;
    std::shared_ptr<output::android::OpenSLAudioSink>            audio_sinks[kAudioChannelCount];
};

struct EngineContext {
    std::unique_ptr<core::AAutoEngine> engine;

    // Pending build state — populated by nativeInit + nativeAdd*/nativeSet*
    // calls, then consumed by nativeFinalizeComposition() to construct
    // the engine. Once finalized, builder calls are ignored.
    core::HeadunitConfig         pending_identity;
    service::ServiceComposition  pending_composition;
    bool                          finalized = false;

    std::mutex                          sessions_mutex;
    std::map<jlong, SessionEntry>       sessions;
    jlong                               next_handle = 1;

    // Touch coordinate scaling. The HU display dimensions match what
    // VideoService advertises in its service definition.
    int view_width    = 1280;
    int view_height   = 720;
    int display_width = 1280;
    int display_height = 720;

    // Java-side service instance for upcalls (PhoneInfo, session closed).
    jobject   service_global = nullptr;
    jmethodID phone_info_method     = nullptr;
    jmethodID session_closed_method = nullptr;
};

std::mutex     g_mutex;
EngineContext* g_ctx = nullptr;
JavaVM*        g_jvm = nullptr;

// ─── Helpers ──────────────────────────────────────────────────────────────────

std::string JStringToStd(JNIEnv* env, jstring s, const char* fallback = "") {
    if (!s) return fallback;
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out(c ? c : fallback);
    if (c) env->ReleaseStringUTFChars(s, c);
    return out;
}

// Look up a session by handle. Returns nullptr if not found.
// Caller must NOT hold g_mutex (this acquires sessions_mutex internally).
std::shared_ptr<session::Session> LookupSession(jlong handle) {
    if (!g_ctx) return nullptr;
    std::lock_guard<std::mutex> lock(g_ctx->sessions_mutex);
    auto it = g_ctx->sessions.find(handle);
    if (it == g_ctx->sessions.end()) return nullptr;
    return it->second.session;
}

// Get a JNIEnv for the current thread, attaching if needed.
struct JniScope {
    JNIEnv* env       = nullptr;
    bool    attached  = false;
    bool    valid     = false;

    JniScope() {
        if (!g_jvm) return;
        if (g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
            if (g_jvm->AttachCurrentThread(&env, nullptr) != 0) return;
            attached = true;
        }
        valid = (env != nullptr);
    }
    ~JniScope() {
        if (attached && g_jvm) g_jvm->DetachCurrentThread();
    }
};

void UpcallPhoneInfo(jlong handle, const session::PhoneInfo& info) {
    JniScope scope;
    if (!scope.valid) return;
    if (!g_ctx || !g_ctx->service_global || !g_ctx->phone_info_method) return;

    jstring jInst  = scope.env->NewStringUTF(info.instance_id.c_str());
    jstring jLife  = scope.env->NewStringUTF(info.connectivity_lifetime_id.c_str());
    jstring jName  = scope.env->NewStringUTF(info.device_name.c_str());
    jstring jLabel = scope.env->NewStringUTF(info.label_text.c_str());

    scope.env->CallVoidMethod(g_ctx->service_global, g_ctx->phone_info_method,
                              handle, jInst, jLife, jName, jLabel);

    scope.env->DeleteLocalRef(jInst);
    scope.env->DeleteLocalRef(jLife);
    scope.env->DeleteLocalRef(jName);
    scope.env->DeleteLocalRef(jLabel);
}

void UpcallSessionClosed(jlong handle) {
    JniScope scope;
    if (!scope.valid) return;
    if (!g_ctx || !g_ctx->service_global || !g_ctx->session_closed_method) return;
    scope.env->CallVoidMethod(g_ctx->service_global, g_ctx->session_closed_method, handle);
}

// Allocate a session entry, build a session over the given transport, and
// return its handle. Returns 0 on failure.
jlong CreateAndStartSession(std::shared_ptr<transport::ITransport> transport) {
    if (!g_ctx || !g_ctx->engine) return 0;

    jlong handle;
    {
        std::lock_guard<std::mutex> lock(g_ctx->sessions_mutex);
        handle = g_ctx->next_handle++;
        g_ctx->sessions[handle] = SessionEntry{};
    }

    core::SessionCallbacks callbacks;
    callbacks.on_phone_info = [handle](const session::PhoneInfo& info) {
        UpcallPhoneInfo(handle, info);
    };
    callbacks.on_closed = [handle]() {
        UpcallSessionClosed(handle);
    };

    auto session = g_ctx->engine->CreateSession(std::move(transport), std::move(callbacks));
    if (!session) {
        std::lock_guard<std::mutex> lock(g_ctx->sessions_mutex);
        g_ctx->sessions.erase(handle);
        return 0;
    }

    {
        std::lock_guard<std::mutex> lock(g_ctx->sessions_mutex);
        g_ctx->sessions[handle].session = session;
    }

    if (!session->Start()) {
        AA_LOG_E() << "Session::Start failed for handle " << handle;
        std::lock_guard<std::mutex> lock(g_ctx->sessions_mutex);
        g_ctx->sessions.erase(handle);
        return 0;
    }

    AA_LOG_I() << "Session started, handle=" << handle;
    return handle;
}

// Helpers to fetch typed services from a session.
std::shared_ptr<service::InputService> GetInput(const std::shared_ptr<session::Session>& s) {
    return std::dynamic_pointer_cast<service::InputService>(
        s->GetService(service::ServiceType::INPUT));
}
std::shared_ptr<service::VideoService> GetVideo(const std::shared_ptr<session::Session>& s) {
    return std::dynamic_pointer_cast<service::VideoService>(
        s->GetService(service::ServiceType::VIDEO));
}
std::shared_ptr<service::AudioService> GetAudio(const std::shared_ptr<session::Session>& s, int channelIdx) {
    auto all = s->GetServicesByType(service::ServiceType::AUDIO);
    if (channelIdx < 0 || channelIdx >= static_cast<int>(all.size())) return nullptr;
    return std::dynamic_pointer_cast<service::AudioService>(all[channelIdx]);
}

}  // namespace

// ─── JNI_OnLoad ───────────────────────────────────────────────────────────────

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    g_jvm = vm;
    return JNI_VERSION_1_6;
}

// ─── JNI implementations ──────────────────────────────────────────────────────

extern "C" {

JNIEXPORT void JNICALL
Java_com_aauto_app_core_AaSessionService_nativeInit(JNIEnv* env, jobject thiz,
                                                      jstring btAddress,
                                                      jint    displayWidth,
                                                      jint    displayHeight,
                                                      jint    displayDensity) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_ctx) {
        AA_LOG_W() << "nativeInit: already initialized";
        return;
    }

    auto* ctx = new EngineContext();

    // Cache the Java service instance + upcall method IDs.
    ctx->service_global = env->NewGlobalRef(thiz);
    jclass cls          = env->GetObjectClass(thiz);
    ctx->phone_info_method = env->GetMethodID(
        cls, "onPhoneInfoFromNative",
        "(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    ctx->session_closed_method = env->GetMethodID(
        cls, "onSessionClosedFromNative", "(J)V");
    env->DeleteLocalRef(cls);

    if (!ctx->phone_info_method || !ctx->session_closed_method) {
        AA_LOG_E() << "Failed to resolve upcall method IDs";
    }

    // Stage the head-unit identity. The service composition is filled in by
    // subsequent nativeAdd*/nativeSet* calls and committed by
    // nativeFinalizeComposition().
    ctx->pending_identity.bluetooth_address = JStringToStd(env, btAddress, "00:11:22:33:44:55");
    ctx->pending_identity.display_width  = displayWidth;
    ctx->pending_identity.display_height = displayHeight;
    ctx->pending_identity.display_density = displayDensity;

    ctx->display_width  = displayWidth;
    ctx->display_height = displayHeight;
    ctx->view_width     = displayWidth;
    ctx->view_height    = displayHeight;

    g_ctx = ctx;
    AA_LOG_I() << "Engine staged, bt_addr=" << ctx->pending_identity.bluetooth_address
               << " display=" << displayWidth << "x" << displayHeight
               << "@" << displayDensity << "dpi";
}

// Append one audio stream to the pending composition.
JNIEXPORT void JNICALL
Java_com_aauto_app_core_AaSessionService_nativeAddAudioStream(JNIEnv* /*env*/, jobject /*thiz*/,
                                                               jint streamType,
                                                               jint sampleRate,
                                                               jint channels) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ctx || g_ctx->finalized) return;

    using aap_protobuf::service::media::sink::message::AudioStreamType;
    const char* name = "Audio";
    switch (streamType) {
        case 1: name = "Audio (Guidance)"; break;
        case 2: name = "Audio (System)";   break;
        case 3: name = "Audio (Media)";    break;
        case 4: name = "Audio (Telephony)"; break;
    }
    g_ctx->pending_composition.audio_streams.push_back(service::AudioServiceConfig{
        static_cast<AudioStreamType>(streamType),
        static_cast<uint32_t>(sampleRate),
        static_cast<uint8_t>(channels),
        /*bits_per_sample=*/16,
        name
    });
}

// Set the video sink service config (single instance).
JNIEXPORT void JNICALL
Java_com_aauto_app_core_AaSessionService_nativeSetVideoConfig(JNIEnv* /*env*/, jobject /*thiz*/,
                                                                jint resolutionEnum,
                                                                jint frameRateEnum,
                                                                jint density,
                                                                jint width,
                                                                jint height,
                                                                jint fps) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ctx || g_ctx->finalized) return;

    using aap_protobuf::service::media::sink::message::VideoCodecResolutionType;
    using aap_protobuf::service::media::sink::message::VideoFrameRateType;

    g_ctx->pending_composition.video = service::VideoServiceConfig{
        static_cast<VideoCodecResolutionType>(resolutionEnum),
        static_cast<VideoFrameRateType>(frameRateEnum),
        /*width_margin=*/0,
        /*height_margin=*/0,
        static_cast<uint32_t>(density),
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        static_cast<uint32_t>(fps)
    };
}

// Set the input source service config (single touch screen).
JNIEXPORT void JNICALL
Java_com_aauto_app_core_AaSessionService_nativeSetInputConfig(JNIEnv* env, jobject /*thiz*/,
                                                                jint touchWidth,
                                                                jint touchHeight,
                                                                jintArray supportedKeycodes) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ctx || g_ctx->finalized) return;

    std::vector<int32_t> keycodes;
    if (supportedKeycodes) {
        jsize n = env->GetArrayLength(supportedKeycodes);
        keycodes.resize(static_cast<size_t>(n));
        if (n > 0) {
            env->GetIntArrayRegion(supportedKeycodes, 0, n, keycodes.data());
        }
    }

    g_ctx->pending_composition.input = service::InputServiceConfig{
        static_cast<uint32_t>(touchWidth),
        static_cast<uint32_t>(touchHeight),
        aap_protobuf::service::inputsource::message::CAPACITIVE,
        /*is_secondary=*/false,
        std::move(keycodes)
    };
}

// Set the sensor source service config (which sensor types this HU exposes).
JNIEXPORT void JNICALL
Java_com_aauto_app_core_AaSessionService_nativeSetSensorConfig(JNIEnv* /*env*/, jobject /*thiz*/,
                                                                 jboolean drivingStatus,
                                                                 jboolean nightMode,
                                                                 jboolean location) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ctx || g_ctx->finalized) return;

    g_ctx->pending_composition.sensor = service::SensorServiceConfig{
        static_cast<bool>(drivingStatus),
        static_cast<bool>(nightMode),
        static_cast<bool>(location)
    };
}

// Set the microphone source service config. Skip the call entirely on
// platforms without a microphone.
JNIEXPORT void JNICALL
Java_com_aauto_app_core_AaSessionService_nativeSetMicrophoneConfig(JNIEnv* /*env*/, jobject /*thiz*/,
                                                                    jint sampleRate,
                                                                    jint channels) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ctx || g_ctx->finalized) return;

    g_ctx->pending_composition.microphone = service::MicrophoneServiceConfig{
        static_cast<uint32_t>(sampleRate),
        static_cast<uint8_t>(channels),
        /*bits_per_sample=*/16
    };
}

// Set the Bluetooth service config (HU's BT MAC address advertised to the phone).
JNIEXPORT void JNICALL
Java_com_aauto_app_core_AaSessionService_nativeSetBluetoothConfig(JNIEnv* env, jobject /*thiz*/,
                                                                    jstring carAddress) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ctx || g_ctx->finalized) return;

    g_ctx->pending_composition.bluetooth = service::BluetoothServiceConfig{
        JStringToStd(env, carAddress, "00:11:22:33:44:55")
    };
}

// Commit the pending identity + composition by constructing the engine.
// After this returns, the builder methods become no-ops and USB / wireless
// transports may be attached.
JNIEXPORT void JNICALL
Java_com_aauto_app_core_AaSessionService_nativeFinalizeComposition(JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ctx || g_ctx->finalized) return;

    g_ctx->engine = std::make_unique<core::AAutoEngine>(
        std::move(g_ctx->pending_identity),
        std::move(g_ctx->pending_composition));
    g_ctx->finalized = true;

    AA_LOG_I() << "Composition finalized; engine ready (bt_addr="
               << g_ctx->engine->GetConfig().bluetooth_address << ")";
}

JNIEXPORT void JNICALL
Java_com_aauto_app_core_AaSessionService_nativeDestroy(JNIEnv* env, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ctx) return;

    // Stop and clear all sessions before destroying the engine.
    {
        std::map<jlong, SessionEntry> sessions;
        {
            std::lock_guard<std::mutex> lk(g_ctx->sessions_mutex);
            sessions = std::move(g_ctx->sessions);
            g_ctx->sessions.clear();
        }
        for (auto& [h, entry] : sessions) {
            if (entry.session) entry.session->Stop();
        }
    }

    if (g_ctx->service_global) {
        env->DeleteGlobalRef(g_ctx->service_global);
        g_ctx->service_global = nullptr;
    }

    delete g_ctx;
    g_ctx = nullptr;
    AA_LOG_I() << "Engine destroyed";
}

JNIEXPORT jlong JNICALL
Java_com_aauto_app_core_AaSessionService_nativeOnUsbDeviceReady(JNIEnv* env, jobject /*thiz*/,
                                                                  jint fd,
                                                                  jint ep_in,
                                                                  jint ep_out,
                                                                  jstring transportId) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ctx) {
        AA_LOG_E() << "nativeOnUsbDeviceReady: engine not initialized";
        return 0;
    }
    std::string id = JStringToStd(env, transportId, "usb_device");

    AA_LOG_I() << "USB device ready: id=" << id << " fd=" << fd
               << " ep_in=0x" << std::hex << ep_in << " ep_out=0x" << ep_out << std::dec;

    auto transport = std::make_shared<transport::AndroidUsbTransport>(
        static_cast<int>(fd), id,
        static_cast<int>(ep_in), static_cast<int>(ep_out));

    return CreateAndStartSession(std::move(transport));
}

JNIEXPORT jlong JNICALL
Java_com_aauto_app_core_AaSessionService_nativeOnWirelessDeviceReady(JNIEnv* env, jobject /*thiz*/,
                                                                       jstring transportId,
                                                                       jobject serverFd) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ctx) {
        AA_LOG_E() << "nativeOnWirelessDeviceReady: engine not initialized";
        return 0;
    }
    std::string id = JStringToStd(env, transportId, "wireless_device");

    jclass   fd_class = env->FindClass("java/io/FileDescriptor");
    jfieldID fd_field = env->GetFieldID(fd_class, "descriptor", "I");
    jint     raw_fd   = env->GetIntField(serverFd, fd_field);
    int      duped_fd = ::dup(static_cast<int>(raw_fd));
    env->DeleteLocalRef(fd_class);

    if (duped_fd < 0) {
        AA_LOG_E() << "dup(serverFd) failed: " << strerror(errno);
        return 0;
    }

    AA_LOG_I() << "Wireless device ready: id=" << id
               << " server_fd=" << raw_fd << " duped_fd=" << duped_fd;

    auto transport = std::make_shared<transport::TcpTransport>(duped_fd, id);
    return CreateAndStartSession(std::move(transport));
}

JNIEXPORT void JNICALL
Java_com_aauto_app_core_AaSessionService_nativeStopSession(JNIEnv* /*env*/, jobject /*thiz*/,
                                                             jlong handle) {
    SessionEntry entry;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_ctx) return;
        std::lock_guard<std::mutex> lk(g_ctx->sessions_mutex);
        auto it = g_ctx->sessions.find(handle);
        if (it == g_ctx->sessions.end()) return;
        entry = std::move(it->second);
        g_ctx->sessions.erase(it);
    }
    AA_LOG_I() << "Stopping session handle=" << handle;
    if (entry.session) entry.session->Stop();
    // entry destructors release sinks (decoder + audio pipelines).
}

JNIEXPORT void JNICALL
Java_com_aauto_app_core_AaSessionService_nativeAttachVideo(JNIEnv* env, jobject /*thiz*/,
                                                             jlong handle, jobject surface) {
    if (!g_ctx || !surface) return;
    auto session = LookupSession(handle);
    if (!session) {
        AA_LOG_W() << "nativeAttachVideo: no session for handle " << handle;
        return;
    }
    auto video = GetVideo(session);
    if (!video) {
        AA_LOG_W() << "nativeAttachVideo: no VideoService";
        return;
    }

    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (!window) {
        AA_LOG_E() << "ANativeWindow_fromSurface returned null";
        return;
    }

    auto sink = std::make_shared<output::android::MediaCodecVideoSink>(window);
    ANativeWindow_release(window);  // sink acquired its own reference

    {
        std::lock_guard<std::mutex> lk(g_ctx->sessions_mutex);
        auto it = g_ctx->sessions.find(handle);
        if (it == g_ctx->sessions.end()) return;
        it->second.video_sink = sink;
    }

    video->SetSink(sink);
    AA_LOG_I() << "Video attached for handle=" << handle;
}

JNIEXPORT void JNICALL
Java_com_aauto_app_core_AaSessionService_nativeDetachVideo(JNIEnv* /*env*/, jobject /*thiz*/,
                                                             jlong handle) {
    if (!g_ctx) return;
    auto session = LookupSession(handle);
    if (!session) return;

    if (auto video = GetVideo(session)) {
        video->SetSink(nullptr);
    }
    {
        std::lock_guard<std::mutex> lk(g_ctx->sessions_mutex);
        auto it = g_ctx->sessions.find(handle);
        if (it != g_ctx->sessions.end()) it->second.video_sink.reset();
    }
    AA_LOG_I() << "Video detached for handle=" << handle;
}

JNIEXPORT void JNICALL
Java_com_aauto_app_core_AaSessionService_nativeAttachAudio(JNIEnv* /*env*/, jobject /*thiz*/,
                                                             jlong handle, jint channelIdx) {
    if (!g_ctx) return;
    auto session = LookupSession(handle);
    if (!session) return;
    auto audio = GetAudio(session, channelIdx);
    if (!audio) {
        AA_LOG_W() << "nativeAttachAudio: no AudioService at idx " << channelIdx;
        return;
    }

    auto sink = std::make_shared<output::android::OpenSLAudioSink>();

    {
        std::lock_guard<std::mutex> lk(g_ctx->sessions_mutex);
        auto it = g_ctx->sessions.find(handle);
        if (it == g_ctx->sessions.end()) return;
        if (channelIdx < 0 || channelIdx >= kAudioChannelCount) return;
        it->second.audio_sinks[channelIdx] = sink;
    }

    audio->SetSink(sink);
    AA_LOG_I() << "Audio attached handle=" << handle << " ch=" << channelIdx;
}

JNIEXPORT void JNICALL
Java_com_aauto_app_core_AaSessionService_nativeDetachAudio(JNIEnv* /*env*/, jobject /*thiz*/,
                                                             jlong handle, jint channelIdx) {
    if (!g_ctx) return;
    auto session = LookupSession(handle);
    if (!session) return;
    auto audio = GetAudio(session, channelIdx);
    if (audio) audio->SetSink(nullptr);

    {
        std::lock_guard<std::mutex> lk(g_ctx->sessions_mutex);
        auto it = g_ctx->sessions.find(handle);
        if (it == g_ctx->sessions.end()) return;
        if (channelIdx < 0 || channelIdx >= kAudioChannelCount) return;
        it->second.audio_sinks[channelIdx].reset();
    }
    AA_LOG_I() << "Audio detached handle=" << handle << " ch=" << channelIdx;
}

JNIEXPORT void JNICALL
Java_com_aauto_app_core_AaSessionService_nativeSetViewSize(JNIEnv* /*env*/, jobject /*thiz*/,
                                                             jint width, jint height) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_ctx || width <= 0 || height <= 0) return;
    g_ctx->view_width  = static_cast<int>(width);
    g_ctx->view_height = static_cast<int>(height);
}

JNIEXPORT void JNICALL
Java_com_aauto_app_core_AaSessionService_nativeDispatchTouchEvent(JNIEnv* /*env*/, jobject /*thiz*/,
                                                                    jlong handle,
                                                                    jint pointer_id,
                                                                    jfloat x, jfloat y,
                                                                    jint action) {
    if (!g_ctx) return;
    auto session = LookupSession(handle);
    if (!session) return;
    auto input = GetInput(session);
    if (!input) return;

    int vw, vh, dw, dh;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        vw = g_ctx->view_width;  vh = g_ctx->view_height;
        dw = g_ctx->display_width; dh = g_ctx->display_height;
    }
    int mapped_x = static_cast<int>(x * dw / (vw > 0 ? vw : 1));
    int mapped_y = static_cast<int>(y * dh / (vh > 0 ? vh : 1));
    input->SendTouchEvent(mapped_x, mapped_y, static_cast<int>(pointer_id), static_cast<int>(action));
}

}  // extern "C"
