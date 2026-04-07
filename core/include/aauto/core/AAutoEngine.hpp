#pragma once

#include <memory>

#include "aauto/core/HeadunitConfig.hpp"
#include "aauto/session/Session.hpp"
#include "aauto/transport/ITransport.hpp"

namespace aauto {
namespace core {

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
    // Caller installs sinks / additional callbacks then calls session->Start().
    std::shared_ptr<session::Session> CreateSession(
        std::shared_ptr<transport::ITransport> transport,
        session::SessionCallbacks              callbacks = {});

    const HeadunitConfig& GetConfig() const { return config_; }

   private:
    HeadunitConfig config_;
};

} // namespace core
} // namespace aauto
