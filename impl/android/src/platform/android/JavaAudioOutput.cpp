#define LOG_TAG "AA.JavaAudioOutput"

#include "aauto/platform/android/JavaAudioOutput.hpp"
#include "aauto/utils/Logger.hpp"

namespace aauto {
namespace platform {
namespace android {

static const char* kJavaClass = "com/aauto/app/media/JavaAudioOutput";

JavaAudioOutput::JavaAudioOutput(JavaVM* jvm)
    : jvm_(jvm) {
    JNIEnv* env = GetEnv();
    if (!env) return;

    jclass cls = env->FindClass(kJavaClass);
    if (!cls) { AA_LOG_E() << "Class not found: " << kJavaClass; return; }

    jmethodID ctor = env->GetMethodID(cls, "<init>", "()V");
    mid_open_  = env->GetMethodID(cls, "open",     "(III)Z");
    mid_start_ = env->GetMethodID(cls, "start",    "()V");
    mid_push_  = env->GetMethodID(cls, "pushData", "([B)V");
    mid_close_ = env->GetMethodID(cls, "close",    "()V");

    jobject local = env->NewObject(cls, ctor);
    java_obj_ = env->NewGlobalRef(local);
    env->DeleteLocalRef(local);
    env->DeleteLocalRef(cls);
}

JavaAudioOutput::~JavaAudioOutput() {
    Close();
    JNIEnv* env = GetEnv();
    if (env && java_obj_) {
        env->DeleteGlobalRef(java_obj_);
        java_obj_ = nullptr;
    }
}

bool JavaAudioOutput::Open(uint32_t sample_rate, uint8_t channels, uint8_t bits) {
    JNIEnv* env = GetEnv();
    if (!env || !java_obj_) return false;
    jboolean ok = env->CallBooleanMethod(java_obj_, mid_open_,
                                         (jint)sample_rate,
                                         (jint)channels,
                                         (jint)bits);
    return ok == JNI_TRUE;
}

void JavaAudioOutput::Start() {
    JNIEnv* env = GetEnv();
    if (!env || !java_obj_) return;
    env->CallVoidMethod(java_obj_, mid_start_);
}

void JavaAudioOutput::Close() {
    JNIEnv* env = GetEnv();
    if (!env || !java_obj_) return;
    env->CallVoidMethod(java_obj_, mid_close_);
}

void JavaAudioOutput::PushAudioData(const std::vector<uint8_t>& data) {
    JNIEnv* env = GetEnv();
    if (!env || !java_obj_) return;

    jbyteArray arr = env->NewByteArray(static_cast<jsize>(data.size()));
    env->SetByteArrayRegion(arr, 0, data.size(),
                            reinterpret_cast<const jbyte*>(data.data()));
    env->CallVoidMethod(java_obj_, mid_push_, arr);
    env->DeleteLocalRef(arr);
}

JNIEnv* JavaAudioOutput::GetEnv() {
    if (!jvm_) return nullptr;
    JNIEnv* env = nullptr;
    jint rc = jvm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        jvm_->AttachCurrentThread(&env, nullptr);
    }
    return env;
}

}  // namespace android
}  // namespace platform
}  // namespace aauto
