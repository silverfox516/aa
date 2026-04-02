#define LOG_TAG "AlsaAudioOutput"
#include "aauto/platform/alsa/AlsaAudioOutput.hpp"
#include "aauto/utils/Logger.hpp"

#include <alsa/asoundlib.h>

namespace aauto {
namespace platform {
namespace alsa {

AlsaAudioOutput::~AlsaAudioOutput() {
    Close();
}

bool AlsaAudioOutput::Open(uint32_t sample_rate, uint8_t channels, uint8_t bits) {
    if (handle_) return true;

    int rc = snd_pcm_open(&handle_, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        AA_LOG_E() << "[AlsaAudioOutput] snd_pcm_open failed: " << snd_strerror(rc);
        return false;
    }

    snd_pcm_format_t fmt;
    switch (bits) {
        case 16: fmt = SND_PCM_FORMAT_S16_LE; break;
        case 8:  fmt = SND_PCM_FORMAT_S8;     break;
        default:
            AA_LOG_E() << "[AlsaAudioOutput] Unsupported bit depth: " << (int)bits;
            snd_pcm_close(handle_);
            handle_ = nullptr;
            return false;
    }

    rc = snd_pcm_set_params(handle_, fmt,
                            SND_PCM_ACCESS_RW_INTERLEAVED,
                            channels,
                            sample_rate,
                            1,       // allow resampling
                            100000); // latency: 100ms
    if (rc < 0) {
        AA_LOG_E() << "[AlsaAudioOutput] snd_pcm_set_params failed: " << snd_strerror(rc);
        snd_pcm_close(handle_);
        handle_ = nullptr;
        return false;
    }

    AA_LOG_I() << "[AlsaAudioOutput] Opened - "
               << sample_rate << "Hz / " << (int)channels << "ch / " << (int)bits << "bit";
    return true;
}

void AlsaAudioOutput::Close() {
    if (handle_) {
        snd_pcm_drain(handle_);
        snd_pcm_close(handle_);
        handle_ = nullptr;
        AA_LOG_I() << "[AlsaAudioOutput] Closed";
    }
}

void AlsaAudioOutput::PushAudioData(const std::vector<uint8_t>& data) {
    if (!handle_ || data.empty()) return;

    snd_pcm_sframes_t frame_bytes = snd_pcm_frames_to_bytes(handle_, 1);
    if (frame_bytes <= 0) return;

    snd_pcm_uframes_t frames = data.size() / static_cast<size_t>(frame_bytes);
    const uint8_t* ptr = data.data();

    while (frames > 0) {
        snd_pcm_sframes_t written = snd_pcm_writei(handle_, ptr, frames);
        if (written == -EPIPE) {
            // Recover from buffer underrun
            snd_pcm_recover(handle_, written, 0);
        } else if (written < 0) {
            AA_LOG_W() << "[AlsaAudioOutput] snd_pcm_writei failed: " << snd_strerror(written);
            break;
        } else {
            ptr    += written * frame_bytes;
            frames -= static_cast<snd_pcm_uframes_t>(written);
        }
    }
}

} // namespace alsa
} // namespace platform
} // namespace aauto
