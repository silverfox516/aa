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

    // Open the audio device with the given PCM format.
    // sample_rate: Hz (e.g. 48000), channels: 1 or 2, bits: 16
    virtual bool Open(uint32_t sample_rate, uint8_t channels, uint8_t bits) = 0;

    // Close the audio device.
    virtual void Close() = 0;

    // Push raw PCM samples. Called from the audio processing thread.
    virtual void PushAudioData(const std::vector<uint8_t>& data) = 0;
};

} // namespace platform
} // namespace aauto
