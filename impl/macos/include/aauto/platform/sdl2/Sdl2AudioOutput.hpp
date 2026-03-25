#pragma once

#include "aauto/platform/IAudioOutput.hpp"
#include <SDL2/SDL.h>

namespace aauto {
namespace platform {

// SDL2-based audio output. Uses SDL_QueueAudio for low-latency PCM playback.
class Sdl2AudioOutput : public IAudioOutput {
   public:
    Sdl2AudioOutput() = default;
    ~Sdl2AudioOutput() override;

    bool Open(uint32_t sample_rate, uint8_t channels, uint8_t bits) override;
    void Close() override;
    void PushAudioData(const std::vector<uint8_t>& data) override;

   private:
    SDL_AudioDeviceID device_id_ = 0;
};

} // namespace platform
} // namespace aauto
