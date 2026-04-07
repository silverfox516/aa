#pragma once

#include <cstdint>

#include <android/native_window.h>
#include <media/NdkMediaCodec.h>

#include "aauto/service/VideoSink.hpp"

namespace aauto {
namespace output {
namespace android {

// IVideoSink implementation backed by an NDK MediaCodec H.264 decoder
// rendering directly to an ANativeWindow surface.
//
// Lifetime:
//   ctor:           acquires the surface; codec is NOT created yet
//   OnVideoConfig:  creates + configures + starts the codec on first call,
//                   queues the codec_data blob as the first input buffer,
//                   then on subsequent calls re-queues the new codec_data
//                   inline (the decoder reconfigures from the new SPS/PPS)
//   OnVideoFrame:   queues input + drains output, dropping frames if the
//                   codec has not been opened yet
//   dtor:           stops + releases the codec, releases the surface
//
// Active vs inactive is encoded by lifetime: a non-existent sink object
// means inactive. The VideoService drops frames while no sink is attached.
//
// Threading: callers must serialize OnVideoConfig and OnVideoFrame against
// each other and against the destructor. VideoService satisfies this by
// holding its sink_mutex_ around all sink callbacks.
class MediaCodecVideoSink : public service::IVideoSink {
   public:
    // Takes ownership of one ANativeWindow_acquire reference. The caller
    // remains free to release its own reference after construction.
    explicit MediaCodecVideoSink(ANativeWindow* surface);
    ~MediaCodecVideoSink() override;

    MediaCodecVideoSink(const MediaCodecVideoSink&)            = delete;
    MediaCodecVideoSink& operator=(const MediaCodecVideoSink&) = delete;

    // service::IVideoSink
    void OnVideoConfig(const service::VideoCodecConfig& config) override;
    void OnVideoFrame(const service::VideoFrame& frame) override;

   private:
    bool EnsureCodec(int width, int height);
    void TeardownCodec();

    // Submit one access unit to the decoder. timeout_us controls how long
    // we will wait for an input buffer slot:
    //   0   — non-blocking; drop the access unit if no slot is free
    //         (used for video frames where dropping is acceptable)
    //   >0  — wait up to timeout_us microseconds; used for codec config,
    //         which is critical and must not be dropped (immediately
    //         after AMediaCodec_start the input buffers may not yet be
    //         available)
    void QueueRawInput(const uint8_t* data, size_t size, int64_t pts_us, int64_t timeout_us);

    void DrainOutput();

    ANativeWindow* window_  = nullptr;
    AMediaCodec*   codec_   = nullptr;
    bool           is_open_ = false;

    int64_t  next_pts_us_ = 0;
    uint64_t frames_in_   = 0;
    uint64_t frames_out_  = 0;
};

} // namespace android
} // namespace output
} // namespace aauto
