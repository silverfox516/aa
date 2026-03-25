#pragma once

#include "aauto/service/ServiceBase.hpp"

namespace aauto {
namespace service {

class MicrophoneService : public ServiceBase {
   public:
    MicrophoneService();
    void FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) override;
    void OnChannelOpened(uint8_t channel) override;
    ServiceType GetType() const override { return ServiceType::MIC; }
    std::string GetName() const override { return "MicrophoneService"; }

   private:
    void HandleMicRequest(const std::vector<uint8_t>& payload);
};

} // namespace service
} // namespace aauto
