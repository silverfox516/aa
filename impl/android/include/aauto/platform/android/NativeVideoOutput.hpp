#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <android/native_window.h>

#include "aauto/platform/IVideoOutput.hpp"

namespace aauto {
namespace platform {
namespace android {

/**
 * Hardware-accelerated H.264 video output using NDK MediaCodec.
 *
 * Uses synchronous dequeueInputBuffer/dequeueOutputBuffer, mirroring the
 * Kotlin reference implementation that is confirmed working on TCC803x.
 */
class NativeVideoOutput : public IVideoOutput {
public:
    NativeVideoOutput();
    ~NativeVideoOutput() override;

    /** Must be called before Open(). */
    void SetSurface(ANativeWindow* window);

    // IVideoOutput
    void Open(int width, int height) override;
    void Close() override;
    void PushVideoData(const std::vector<uint8_t>& data, bool is_codec_config) override;
    void SetTouchCallback(TouchCallback cb) override;

private:
    ANativeWindow*    window_  = nullptr;
    AMediaCodec*      codec_   = nullptr;
    std::atomic<bool> is_open_{false};

    int64_t           next_pts_us_ = 0;
    uint64_t          frames_in_   = 0;
    uint64_t          frames_out_ = 0;

    TouchCallback touch_callback_;
    std::mutex    touch_mutex_;
};

}  // namespace android
}  // namespace platform
}  // namespace aauto
