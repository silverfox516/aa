#include "aauto/platform/sdl2/Sdl2VideoOutput.hpp"
#include "aauto/video/VideoRenderer.hpp"

namespace aauto {
namespace platform {
namespace sdl2 {

Sdl2VideoOutput::Sdl2VideoOutput(std::shared_ptr<video::VideoRenderer> renderer)
    : renderer_(std::move(renderer)) {}

void Sdl2VideoOutput::Open(int width, int height) {
    if (renderer_) renderer_->OpenWindow();
}

void Sdl2VideoOutput::Close() {
    if (renderer_) renderer_->CloseWindow();
}

void Sdl2VideoOutput::PushVideoData(const std::vector<uint8_t>& data) {
    if (renderer_) renderer_->PushVideoData(data);
}

void Sdl2VideoOutput::SetTouchCallback(TouchCallback cb) {
    if (!renderer_) return;
    renderer_->SetTouchCallback([cb = std::move(cb)](int x, int y, int pointer_id, int action) {
        cb(TouchEvent{x, y, pointer_id, action});
    });
}

} // namespace sdl2
} // namespace platform
} // namespace aauto
