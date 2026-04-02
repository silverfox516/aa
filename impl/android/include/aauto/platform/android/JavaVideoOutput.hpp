#pragma once

#include <jni.h>
#include <mutex>
#include <vector>

#include <android/native_window.h>
#include <android/native_window_jni.h>

#include "aauto/platform/IVideoOutput.hpp"

namespace aauto {
namespace platform {
namespace android {

/**
 * IVideoOutput bridge to Java MediaCodec via JNI.
 *
 * Delegates open/pushData/close to a JavaVideoOutput Java object.
 * Requires a JavaVM* and a Surface jobject before Open() is called.
 */
class JavaVideoOutput : public IVideoOutput {
public:
    JavaVideoOutput(JavaVM* jvm, jobject surface);
    ~JavaVideoOutput() override;

    // IVideoOutput
    void Open(int width, int height) override;
    void Close() override;
    void PushVideoData(const std::vector<uint8_t>& data) override;
    void SetTouchCallback(TouchCallback cb) override;

private:
    JNIEnv* GetEnv();

    JavaVM*   jvm_;
    jobject   java_obj_    = nullptr;  // GlobalRef to JavaVideoOutput instance
    jobject   surface_ref_ = nullptr;  // GlobalRef to Surface

    jmethodID mid_open_     = nullptr;
    jmethodID mid_push_     = nullptr;
    jmethodID mid_close_    = nullptr;
    jmethodID mid_set_surf_ = nullptr;

    TouchCallback touch_callback_;
    std::mutex    touch_mutex_;
};

}  // namespace android
}  // namespace platform
}  // namespace aauto
