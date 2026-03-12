#pragma once

#include <functional>
#include <memory>

#include "aauto/core/HeadunitConfig.hpp"
#include "aauto/service/ServiceBase.hpp"

namespace aauto {
namespace service {

class ControlService : public ServiceBase {
   public:
    explicit ControlService(core::HeadunitConfig config = {});

    void HandleMessage(uint16_t msg_type, const std::vector<uint8_t>& payload) override;
    void FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) override {}
    ServiceType GetType() const override { return ServiceType::CONTROL; }
    std::string GetName() const override { return "ControlService"; }

    using ServiceProvider = std::function<std::vector<std::shared_ptr<IService>>()>;
    void SetServiceProvider(ServiceProvider provider) { service_provider_ = std::move(provider); }

    void SendAudioFocusNotification(int state);
    void SendNavFocusNotification(int type);

   private:
    void SendServiceDiscoveryResponse();

    core::HeadunitConfig config_;
    ServiceProvider      service_provider_;
};

} // namespace service
} // namespace aauto
