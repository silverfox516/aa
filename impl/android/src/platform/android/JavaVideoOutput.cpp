#define LOG_TAG "AA.JavaVideoOutput"

#include "aauto/platform/android/JavaVideoOutput.hpp"
#include "aauto/utils/Logger.hpp"

namespace aauto {
namespace platform {
namespace android {

static const char* kJavaClass = "com/aauto/app/media/JavaVideoOutput";

JavaVideoOutput::JavaVideoOutput(JavaVM* jvm, jobject surface)
    : jvm_(jvm) {
    JNIEnv* env = GetEnv();
    if (!env) return;

    if (surface) {
        surface_ref_ = env->NewGlobalRef(surface);
    }

    jclass cls = env->FindClass(kJavaClass);
    if (!cls) { AA_LOG_E() << "Class not found: " << kJavaClass; return; }

    jmethodID ctor = env->GetMethodID(cls, "<init>", "()V");
    mid_open_     = env->GetMethodID(cls, "open",       "(II)Z");
    mid_push_     = env->GetMethodID(cls, "pushData",   "([B)V");
    mid_close_    = env->GetMethodID(cls, "close",      "()V");
    mid_set_surf_ = env->GetMethodID(cls, "setSurface", "(Landroid/view/Surface;)V");

    jobject local = env->NewObject(cls, ctor);
    java_obj_ = env->NewGlobalRef(local);
    env->DeleteLocalRef(local);
    env->DeleteLocalRef(cls);
}

JavaVideoOutput::~JavaVideoOutput() {
    Close();
    JNIEnv* env = GetEnv();
    if (!env) return;
    if (java_obj_)    { env->DeleteGlobalRef(java_obj_);    java_obj_    = nullptr; }
    if (surface_ref_) { env->DeleteGlobalRef(surface_ref_); surface_ref_ = nullptr; }
}

void JavaVideoOutput::Open(int width, int height) {
    JNIEnv* env = GetEnv();
    if (!env || !java_obj_) return;

    if (surface_ref_ && mid_set_surf_) {
        env->CallVoidMethod(java_obj_, mid_set_surf_, surface_ref_);
    }

    jboolean ok = env->CallBooleanMethod(java_obj_, mid_open_,
                                         (jint)width, (jint)height);
    if (!ok) AA_LOG_E() << "JavaVideoOutput.open() returned false";
}

void JavaVideoOutput::Close() {
    JNIEnv* env = GetEnv();
    if (!env || !java_obj_) return;
    env->CallVoidMethod(java_obj_, mid_close_);
}

void JavaVideoOutput::PushVideoData(const std::vector<uint8_t>& data) {
    JNIEnv* env = GetEnv();
    if (!env || !java_obj_) return;

    jbyteArray arr = env->NewByteArray(static_cast<jsize>(data.size()));
    env->SetByteArrayRegion(arr, 0, data.size(),
                            reinterpret_cast<const jbyte*>(data.data()));
    env->CallVoidMethod(java_obj_, mid_push_, arr);
    env->DeleteLocalRef(arr);
}

void JavaVideoOutput::SetTouchCallback(TouchCallback cb) {
    std::lock_guard<std::mutex> lock(touch_mutex_);
    touch_callback_ = std::move(cb);
}

JNIEnv* JavaVideoOutput::GetEnv() {
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
