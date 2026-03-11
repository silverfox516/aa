#pragma once

#include "aauto/service/IService.hpp"

namespace aauto {
namespace service {

class SensorService : public IService {
   public:
    void HandleMessage(uint16_t msg_type, const std::vector<uint8_t>& payload) override;
    void SetSendCallback(SendCallback cb) override { send_cb_ = cb; }
    void FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) override;
    void OnChannelOpened(uint8_t channel) override;
    std::vector<uint8_t> PrepareMessage(const std::vector<uint8_t>& payload) override { return payload; }
    ServiceType GetType() const override { return ServiceType::SENSOR; }
    std::string GetName() const override { return "SensorService"; }

   private:
    void HandleSensorStartRequest(const std::vector<uint8_t>& payload);
    void SendDrivingStatus();

    SendCallback send_cb_;
};

}  // namespace service
}  // namespace aauto
