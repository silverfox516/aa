#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <vector>

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include "aauto/platform/IAudioOutput.hpp"

namespace aauto {
namespace platform {
namespace android {

/**
 * PCM audio output using OpenSL ES (Android NDK).
 *
 * Uses the Android simple buffer queue extension for low-latency
 * streaming playback. PushAudioData() enqueues PCM buffers directly
 * into the SL buffer queue.
 *
 * OpenSL ES is chosen over AAudio for broader Android version support
 * (AAudio requires API 26+; OpenSL ES is available from API 9).
 */
class NativeAudioOutput : public IAudioOutput {
public:
    NativeAudioOutput();
    ~NativeAudioOutput() override;

    // IAudioOutput
    bool Open(uint32_t sample_rate, uint8_t channels, uint8_t bits) override;
    void Start() override;
    void Close() override;
    void PushAudioData(const std::vector<uint8_t>& data) override;

private:
    static void BufferQueueCallback(SLAndroidSimpleBufferQueueItf bq, void* context);
    void OnBufferConsumed();
    void DrainPending();  // caller must hold queue_mutex_

    SLObjectItf  engine_obj_   = nullptr;
    SLEngineItf  engine_       = nullptr;
    SLObjectItf  mix_obj_      = nullptr;
    SLObjectItf  player_obj_   = nullptr;
    SLPlayItf    player_       = nullptr;
    SLAndroidSimpleBufferQueueItf buffer_queue_ = nullptr;

    std::atomic<bool> is_open_{false};

    // Double-buffering: two slots in the OpenSL queue.
    // live_ holds the vectors currently owned by OpenSL (must not be modified
    // until the callback fires). write_idx_ cycles through slots in FIFO order.
    static constexpr size_t kNumBuffers = 2;
    static constexpr size_t kMaxPending = 16;  // drop oldest if backlog grows

    std::vector<uint8_t> live_[kNumBuffers];
    int                  write_idx_ = 0;
    int                  enqueued_  = 0;

    std::mutex                       queue_mutex_;
    std::deque<std::vector<uint8_t>> pending_;
};

}  // namespace android
}  // namespace platform
}  // namespace aauto
