#pragma once

#include <functional>
#include <memory>

#include "aauto/core/HeadunitConfig.hpp"
#include "aauto/session/PhoneInfo.hpp"
#include "aauto/session/Session.hpp"
#include "aauto/transport/ITransport.hpp"

namespace aauto {
namespace core {

// Lifecycle notifications observed by the app layer over a session.
//
// Defined at the engine layer (rather than inside Session) because the
// engine is the seam where app-layer concerns are routed into the right
// internal owner: on_phone_info is consumed by ControlService at
// construction time, while on_closed is consumed by Session itself.
struct SessionCallbacks {
    // Fired when the phone identifies itself in ServiceDiscoveryRequest.
    // Called on the session process thread.
    std::function<void(const session::PhoneInfo&)> on_phone_info;

    // Fired exactly once when the session ends (transport disconnect or
    // explicit Stop). Called on the receive thread or the thread that
    // invoked Stop. Must not block.
    std::function<void()> on_closed;
};

// Stateless factory for AAP sessions. Holds the head-unit configuration
// (vehicle identity, display capabilities, BT address, etc.) and uses it
// to construct fully wired Session objects from a connected transport.
//
// AAutoEngine has no knowledge of platform output objects. Sessions emit
// data through sinks attached later by the caller.
class AAutoEngine {
   public:
    explicit AAutoEngine(HeadunitConfig config);
    ~AAutoEngine() = default;

    AAutoEngine(const AAutoEngine&)            = delete;
    AAutoEngine& operator=(const AAutoEngine&) = delete;

    // Build (but do not start) a new session over the given transport.
    // Caller installs sinks then calls session->Start().
    std::shared_ptr<session::Session> CreateSession(
        std::shared_ptr<transport::ITransport> transport,
        SessionCallbacks                       callbacks = {});

    const HeadunitConfig& GetConfig() const { return config_; }

   private:
    HeadunitConfig config_;
};

} // namespace core
} // namespace aauto
