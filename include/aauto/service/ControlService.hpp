#pragma once

#include "aauto/service/IService.hpp"
#include <functional>
#include <memory>

namespace aauto {
namespace service {

class ControlService : public IService {
   public:
    void HandleMessage(uint16_t msg_type, const std::vector<uint8_t>& payload) override;
    void SetSendCallback(SendCallback cb) override { send_cb_ = cb; }
    
    using ServiceProvider = std::function<std::vector<std::shared_ptr<IService>>()>;
    void SetServiceProvider(ServiceProvider provider) { service_provider_ = std::move(provider); }

    void SendAudioFocusNotification(int state);
    void SendNavFocusNotification(int type);

    void FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) override {}
    std::vector<uint8_t> PrepareMessage(const std::vector<uint8_t>& payload) override { return payload; }
    ServiceType GetType() const override { return ServiceType::CONTROL; }
    std::string GetName() const override { return "ControlService"; }

   private:
    void SendServiceDiscoveryResponse();

    SendCallback send_cb_;
    ServiceProvider service_provider_;
};

}  // namespace service
}  // namespace aauto
