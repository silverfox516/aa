#pragma once

#include <cstdint>
#include <vector>

namespace aauto {
namespace platform {

// Abstract audio output sink.
// Implementations: Sdl2AudioOutput, or headless/null for testing.
class IAudioOutput {
   public:
    virtual ~IAudioOutput() = default;

    // Prepare the audio device (pipeline creation, PAUSED state).
    // Called on MEDIA_SETUP — slow initialization happens here.
    virtual bool Open(uint32_t sample_rate, uint8_t channels, uint8_t bits) = 0;

    // Start playback (PAUSED → PLAYING).
    // Called on MEDIA_START — must return quickly.
    virtual void Start() {}

    // Close the audio device.
    virtual void Close() = 0;

    // Push raw PCM samples. Called from the audio processing thread.
    virtual void PushAudioData(const std::vector<uint8_t>& data) = 0;
};

} // namespace platform
} // namespace aauto
