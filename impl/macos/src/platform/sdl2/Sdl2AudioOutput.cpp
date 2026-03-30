#define LOG_TAG "Sdl2AudioOutput"
#include "aauto/platform/sdl2/Sdl2AudioOutput.hpp"
#include "aauto/utils/Logger.hpp"

namespace aauto {
namespace platform {

Sdl2AudioOutput::~Sdl2AudioOutput() {
    Close();
}

bool Sdl2AudioOutput::Open(uint32_t sample_rate, uint8_t channels, uint8_t bits) {
    if (device_id_ != 0) return true;  // already open

    if (!(SDL_WasInit(0) & SDL_INIT_AUDIO)) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            AA_LOG_E() << "[Sdl2AudioOutput] SDL_INIT_AUDIO 초기화 실패: " << SDL_GetError();
            return false;
        }
    }

    SDL_AudioSpec desired{};
    desired.freq     = static_cast<int>(sample_rate);
    desired.channels = channels;
    desired.samples  = 1024;
    desired.callback = nullptr;  // push mode (SDL_QueueAudio)

    switch (bits) {
        case 16: desired.format = AUDIO_S16SYS; break;
        case 8:  desired.format = AUDIO_S8;     break;
        default:
            AA_LOG_E() << "[Sdl2AudioOutput] 지원하지 않는 비트 수: " << (int)bits;
            return false;
    }

    SDL_AudioSpec obtained{};
    device_id_ = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    if (device_id_ == 0) {
        AA_LOG_E() << "[Sdl2AudioOutput] SDL_OpenAudioDevice 실패: " << SDL_GetError();
        return false;
    }

    // Open in paused state; Start() will unpause when MEDIA_START arrives
    SDL_PauseAudioDevice(device_id_, 1);
    AA_LOG_I() << "[Sdl2AudioOutput] 오픈 완료 (paused) - "
               << obtained.freq << "Hz / " << (int)obtained.channels << "ch";
    return true;
}

void Sdl2AudioOutput::Start() {
    if (device_id_ != 0) {
        SDL_PauseAudioDevice(device_id_, 0);
        AA_LOG_I() << "[Sdl2AudioOutput] 재생 시작";
    }
}

void Sdl2AudioOutput::Close() {
    if (device_id_ != 0) {
        SDL_CloseAudioDevice(device_id_);
        device_id_ = 0;
        AA_LOG_I() << "[Sdl2AudioOutput] 닫힘";
    }
}

void Sdl2AudioOutput::PushAudioData(const std::vector<uint8_t>& data) {
    if (device_id_ == 0 || data.empty()) return;

    if (SDL_QueueAudio(device_id_, data.data(), static_cast<Uint32>(data.size())) != 0) {
        AA_LOG_W() << "[Sdl2AudioOutput] SDL_QueueAudio 실패: " << SDL_GetError();
    }
}

} // namespace platform
} // namespace aauto
