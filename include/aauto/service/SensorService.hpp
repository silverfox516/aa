#pragma once

#include "aauto/service/ServiceBase.hpp"

namespace aauto {
namespace service {

class SensorService : public ServiceBase {
   public:
    void HandleMessage(uint16_t msg_type, const std::vector<uint8_t>& payload) override;
    void FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) override;
    void OnChannelOpened(uint8_t channel) override;
    ServiceType GetType() const override { return ServiceType::SENSOR; }
    std::string GetName() const override { return "SensorService"; }

   private:
    void HandleSensorStartRequest(const std::vector<uint8_t>& payload);
    void SendDrivingStatus();
};

} // namespace service
} // namespace aauto
