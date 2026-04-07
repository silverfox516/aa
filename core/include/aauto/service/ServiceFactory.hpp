#pragma once

#include <memory>
#include <vector>

#include "aauto/core/HeadunitConfig.hpp"
#include "aauto/service/IService.hpp"

namespace aauto {
namespace service {

// All external dependencies needed to construct services.
// Services emit data through sinks attached later by the app layer, so
// no platform output objects are required at construction time.
struct ServiceContext {
    core::HeadunitConfig config;
};

// Creates and wires all services for a session.
// No static state — safe for multiple concurrent sessions.
class ServiceFactory {
   public:
    explicit ServiceFactory(ServiceContext context);

    // Build the full set of services for one AA session.
    std::vector<std::shared_ptr<IService>> CreateAll() const;

   private:
    std::shared_ptr<IService> CreateControl(const std::vector<std::shared_ptr<IService>>& peers) const;
    std::shared_ptr<IService> CreateAudioMedia()      const;
    std::shared_ptr<IService> CreateAudioGuidance()   const;
    std::shared_ptr<IService> CreateAudioSystem()     const;
    std::shared_ptr<IService> CreateVideo()           const;
    std::shared_ptr<IService> CreateInput()           const;
    std::shared_ptr<IService> CreateSensor()          const;
    std::shared_ptr<IService> CreateMicrophone()      const;
    std::shared_ptr<IService> CreateBluetooth()       const;

    ServiceContext ctx_;
};

} // namespace service
} // namespace aauto
