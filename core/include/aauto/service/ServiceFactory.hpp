#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "aauto/core/HeadunitConfig.hpp"
#include "aauto/service/IService.hpp"
#include "aauto/service/ServiceComposition.hpp"
#include "aauto/session/PhoneInfo.hpp"

namespace aauto {
namespace service {

// All external dependencies needed to construct services.
//
// `identity` carries head-unit identity (vehicle make/model/etc) used in
// the discovery response. `composition` declares which services this
// platform exposes and with what options — only services present in
// `composition` are created and advertised. `phone_info_cb` is wired into
// ControlService at construction time so the session lifecycle layer
// never has to know about service-specific notification interfaces.
struct ServiceContext {
    core::HeadunitConfig                            identity;
    ServiceComposition                              composition;
    std::function<void(const session::PhoneInfo&)>  phone_info_cb;
};

// Creates and wires the services declared by ctx.composition for one
// AA session. No static state — safe for multiple concurrent sessions.
class ServiceFactory {
   public:
    explicit ServiceFactory(ServiceContext context);

    // Build the full set of services (control + the peers requested by
    // the composition).
    std::vector<std::shared_ptr<IService>> CreateAll() const;

   private:
    std::shared_ptr<IService> CreateControl(const std::vector<std::shared_ptr<IService>>& peers) const;

    ServiceContext ctx_;
};

} // namespace service
} // namespace aauto
