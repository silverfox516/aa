#pragma once

#include "aauto/platform/IAudioOutput.hpp"

// Forward-declare opaque ALSA type to avoid pulling in <alsa/asoundlib.h> in this header.
struct _snd_pcm;
typedef struct _snd_pcm snd_pcm_t;

namespace aauto {
namespace platform {
namespace alsa {

// ALSA PCM playback implementation of IAudioOutput.
class AlsaAudioOutput : public IAudioOutput {
   public:
    AlsaAudioOutput() = default;
    ~AlsaAudioOutput() override;

    bool Open(uint32_t sample_rate, uint8_t channels, uint8_t bits) override;
    void Close() override;
    void PushAudioData(const std::vector<uint8_t>& data) override;

   private:
    snd_pcm_t* handle_ = nullptr;
};

} // namespace alsa
} // namespace platform
} // namespace aauto
