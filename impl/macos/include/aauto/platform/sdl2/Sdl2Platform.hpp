#pragma once

#include <memory>
#include <string>

#include "aauto/platform/IPlatform.hpp"

namespace aauto {
namespace platform {
namespace sdl2 {

class Sdl2VideoOutput;

struct Sdl2Config {
    int         width  = 1280;
    int         height = 720;
    std::string title  = "Android Auto";
};

// SDL2 platform: owns Sdl2VideoOutput, drives its event loop on the main thread.
class Sdl2Platform : public IPlatform {
public:
    explicit Sdl2Platform(Sdl2Config config = {});

    bool Initialize() override;
    std::shared_ptr<IVideoOutput>  GetVideoOutput() override;
    std::shared_ptr<IAudioOutput>  GetAudioOutput() override;
    // Returns a new Sdl2AudioOutput per call so each audio stream
    // (media/guidance/system) gets its own SDL audio device.
    std::shared_ptr<IAudioOutput>  CreateAudioOutput() override;
    void Run() override;
    void Stop() override;

private:
    Sdl2Config config_;
    std::shared_ptr<Sdl2VideoOutput>  video_output_;
    std::shared_ptr<IAudioOutput>     audio_output_;
};

} // namespace sdl2
} // namespace platform
} // namespace aauto
