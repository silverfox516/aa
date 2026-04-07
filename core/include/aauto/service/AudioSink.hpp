#pragma once

#include <cstddef>
#include <cstdint>

namespace aauto {
namespace service {

// AAP carries multiple independent audio streams (media / guidance / system).
// Each is exposed as a separate AudioService instance, so an audio sink does
// not need to know which stream it serves — that is determined by which
// AudioService it is attached to.
struct AudioFormat {
    uint32_t sample_rate     = 0;   // e.g. 48000
    uint8_t  channel_count   = 0;   // 1 = mono, 2 = stereo
    uint8_t  bits_per_sample = 0;   // typically 16
};

// Sink interface implemented by the consumer (impl/app) to receive
// PCM audio data from an AudioService. Lifetime semantics match
// IVideoSink: attach starts playback, detach stops it.
class IAudioSink {
   public:
    virtual ~IAudioSink() = default;

    // Called once after attach if the service has a cached format,
    // and again whenever the phone re-negotiates the format.
    virtual void OnAudioFormat(const AudioFormat& format) = 0;

    // Called for every received PCM chunk while this sink is attached.
    virtual void OnAudioData(const uint8_t* data, size_t size, uint64_t pts_us) = 0;
};

} // namespace service
} // namespace aauto
