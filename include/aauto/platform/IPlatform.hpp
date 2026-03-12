#pragma once

#include <memory>

#include "aauto/platform/IVideoOutput.hpp"

namespace aauto {
namespace platform {

// Abstract platform layer.
// A Platform provides access to all UI/hardware capabilities that vary
// between SDL2, Qt, Flutter, or headless testing environments.
//
// Usage:
//   auto platform = std::make_shared<Sdl2Platform>();
//   platform->Initialize();
//   AAutoEngine engine(device_manager, platform);
//   platform->Run();   // blocks on main thread (SDL2 / Qt event loop)
class IPlatform {
   public:
    virtual ~IPlatform() = default;

    virtual bool Initialize() = 0;

    // Returns the video output surface for the given session/stream.
    // May return the same instance every time (single-window) or create new ones.
    virtual std::shared_ptr<IVideoOutput> GetVideoOutput() = 0;

    // Run the platform event loop on the calling (main) thread.
    // Returns when the user requests exit.
    virtual void Run() = 0;

    // Signal the event loop to stop gracefully.
    virtual void Stop() = 0;
};

} // namespace platform
} // namespace aauto
