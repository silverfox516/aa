#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace aauto {
namespace platform {

// Touch/pointer event delivered to the platform UI.
struct TouchEvent {
    int x;
    int y;
    int pointer_id;
    int action;   // 0=down, 1=up, 2=move
};

using TouchCallback = std::function<void(const TouchEvent&)>;

// Abstract video output surface.
// Implementations: Sdl2VideoOutput, QtVideoOutput, FlutterVideoOutput, ...
class IVideoOutput {
   public:
    virtual ~IVideoOutput() = default;

    // Called when video streaming starts (open/show surface).
    virtual void Open(int width, int height) = 0;
    // Called when video streaming stops (close/hide surface).
    virtual void Close() = 0;

    // Push a raw H.264 NAL unit for decoding and display.
    virtual void PushVideoData(const std::vector<uint8_t>& data) = 0;

    // Register a callback to receive touch events from the UI.
    virtual void SetTouchCallback(TouchCallback cb) = 0;
};

} // namespace platform
} // namespace aauto
