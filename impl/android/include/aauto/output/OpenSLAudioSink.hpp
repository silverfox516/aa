#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include "aauto/service/AudioSink.hpp"

namespace aauto {
namespace output {
namespace android {

// IAudioSink implementation backed by an OpenSL ES PCM player.
//
// Lifetime:
//   ctor:           constructs an empty pipeline; nothing is opened yet
//   OnAudioFormat:  creates the OpenSL engine + player matching the format
//                   and starts playback (first call only — re-format is
//                   currently a no-op since AAP audio formats are stable
//                   per channel)
//   OnAudioData:    enqueues PCM into the buffer queue, dropping the
//                   oldest pending frame if the backlog grows
//   dtor:           stops + destroys all OpenSL objects
class OpenSLAudioSink : public service::IAudioSink {
   public:
    OpenSLAudioSink();
    ~OpenSLAudioSink() override;

    OpenSLAudioSink(const OpenSLAudioSink&)            = delete;
    OpenSLAudioSink& operator=(const OpenSLAudioSink&) = delete;

    // service::IAudioSink
    void OnAudioFormat(const service::AudioFormat& format) override;
    void OnAudioData(const uint8_t* data, size_t size, uint64_t pts_us) override;

   private:
    bool OpenPipeline(uint32_t sample_rate, uint8_t channels, uint8_t bits);
    void ClosePipeline();
    void DrainPending();   // caller must hold queue_mutex_

    static void BufferQueueCallback(SLAndroidSimpleBufferQueueItf bq, void* context);
    void OnBufferConsumed();

    SLObjectItf engine_obj_ = nullptr;
    SLEngineItf engine_     = nullptr;
    SLObjectItf mix_obj_    = nullptr;
    SLObjectItf player_obj_ = nullptr;
    SLPlayItf   player_     = nullptr;
    SLAndroidSimpleBufferQueueItf buffer_queue_ = nullptr;

    std::atomic<bool> is_open_{false};

    static constexpr size_t kNumBuffers = 2;
    static constexpr size_t kMaxPending = 16;

    std::vector<uint8_t> live_[kNumBuffers];
    int                  write_idx_ = 0;
    int                  enqueued_  = 0;

    std::mutex                       queue_mutex_;
    std::deque<std::vector<uint8_t>> pending_;
};

} // namespace android
} // namespace output
} // namespace aauto
