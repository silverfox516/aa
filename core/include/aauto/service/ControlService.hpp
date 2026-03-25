#pragma once

#include <memory>
#include <vector>

#include "aauto/core/HeadunitConfig.hpp"
#include "aauto/service/ServiceBase.hpp"

namespace aauto {
namespace service {

class ControlService : public ServiceBase {
   public:
    ControlService(core::HeadunitConfig config,
                   std::vector<std::shared_ptr<IService>> peer_services);

    void FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) override {}
    ServiceType GetType() const override { return ServiceType::CONTROL; }
    std::string GetName() const override { return "ControlService"; }

    void SendAudioFocusNotification(int state);
    void SendNavFocusNotification(int type);

   private:
    void SendServiceDiscoveryResponse();

    core::HeadunitConfig                   config_;
    std::vector<std::shared_ptr<IService>> peer_services_;
};

} // namespace service
} // namespace aauto
