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

    // Push H.264 NAL data for decoding and display.
    //
    // Android Auto wire format:
    //   MEDIA_CODEC_CONFIG (is_codec_config=true):
    //     payload = [SPS/PPS NAL data]  (no timestamp prefix)
    //   MEDIA_DATA (is_codec_config=false):
    //     payload = [8-byte timestamp][H.264 NAL data]
    //
    // Implementations are responsible for stripping the timestamp header
    // before passing data to the decoder.
    virtual void PushVideoData(const std::vector<uint8_t>& data, bool is_codec_config) = 0;

    // Register a callback to receive touch events from the UI.
    virtual void SetTouchCallback(TouchCallback cb) = 0;
};

} // namespace platform
} // namespace aauto
