#pragma once

#include <jni.h>
#include <vector>

#include "aauto/platform/IAudioOutput.hpp"

namespace aauto {
namespace platform {
namespace android {

/**
 * IAudioOutput bridge to Java AudioTrack via JNI.
 *
 * Delegates open/start/pushData/close to a JavaAudioOutput Java object.
 */
class JavaAudioOutput : public IAudioOutput {
public:
    explicit JavaAudioOutput(JavaVM* jvm);
    ~JavaAudioOutput() override;

    // IAudioOutput
    bool Open(uint32_t sample_rate, uint8_t channels, uint8_t bits) override;
    void Start() override;
    void Close() override;
    void PushAudioData(const std::vector<uint8_t>& data) override;

private:
    JNIEnv* GetEnv();

    JavaVM*   jvm_;
    jobject   java_obj_ = nullptr;

    jmethodID mid_open_  = nullptr;
    jmethodID mid_start_ = nullptr;
    jmethodID mid_push_  = nullptr;
    jmethodID mid_close_ = nullptr;
};

}  // namespace android
}  // namespace platform
}  // namespace aauto
