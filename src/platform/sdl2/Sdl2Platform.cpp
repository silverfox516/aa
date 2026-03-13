#include "aauto/platform/sdl2/Sdl2Platform.hpp"
#include "aauto/platform/sdl2/Sdl2AudioOutput.hpp"
#include "aauto/platform/sdl2/Sdl2VideoOutput.hpp"

namespace aauto {
namespace platform {
namespace sdl2 {

Sdl2Platform::Sdl2Platform(Sdl2Config config)
    : config_(std::move(config)) {}

bool Sdl2Platform::Initialize() {
    auto video = std::make_shared<Sdl2VideoOutput>(
        Sdl2VideoOutput::Config{config_.width, config_.height, config_.title});
    if (!video->Initialize()) return false;

    video_output_ = std::move(video);
    audio_output_ = std::make_shared<Sdl2AudioOutput>();
    return true;
}

std::shared_ptr<IVideoOutput> Sdl2Platform::GetVideoOutput() {
    return video_output_;
}

std::shared_ptr<IAudioOutput> Sdl2Platform::GetAudioOutput() {
    return audio_output_;
}

void Sdl2Platform::Run() {
    if (video_output_) video_output_->Run();
}

void Sdl2Platform::Stop() {
    if (video_output_) video_output_->Stop();
}

} // namespace sdl2
} // namespace platform
} // namespace aauto
