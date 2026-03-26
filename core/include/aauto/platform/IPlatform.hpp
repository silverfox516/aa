#pragma once

#include <memory>

#include "aauto/platform/IAudioOutput.hpp"
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

    virtual std::shared_ptr<IVideoOutput>  GetVideoOutput() = 0;
    virtual std::shared_ptr<IAudioOutput>  GetAudioOutput() = 0;

    // Returns a NEW audio output instance each call.
    // Override in platforms that support independent per-stream pipelines.
    // Default: returns the same shared instance (single-pipeline platforms).
    virtual std::shared_ptr<IAudioOutput>  CreateAudioOutput() { return GetAudioOutput(); }

    // Run the platform event loop on the calling (main) thread.
    // Returns when the user requests exit.
    virtual void Run() = 0;

    // Signal the event loop to stop gracefully.
    virtual void Stop() = 0;
};

} // namespace platform
} // namespace aauto
