#pragma once

#include <memory>

#include "aauto/platform/IVideoOutput.hpp"

namespace aauto {
namespace video { class VideoRenderer; }

namespace platform {
namespace sdl2 {

// SDL2 + FFmpeg implementation of IVideoOutput.
// Wraps the existing VideoRenderer, adapting its interface to IVideoOutput.
class Sdl2VideoOutput : public IVideoOutput {
   public:
    explicit Sdl2VideoOutput(std::shared_ptr<video::VideoRenderer> renderer);

    void Open(int width, int height) override;
    void Close() override;
    void PushVideoData(const std::vector<uint8_t>& data) override;
    void SetTouchCallback(TouchCallback cb) override;

   private:
    std::shared_ptr<video::VideoRenderer> renderer_;
};

} // namespace sdl2
} // namespace platform
} // namespace aauto
