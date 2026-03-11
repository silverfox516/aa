#pragma once
#include "aauto/service/IService.hpp"

namespace aauto {
namespace service {

class BluetoothService : public IService {
public:
    BluetoothService() = default;

    void HandleMessage(uint16_t msg_type, const std::vector<uint8_t>& payload) override {
        HandleChannelOpenRequest(msg_type, payload, send_cb_, channel_);
    }

    void SetSendCallback(SendCallback cb) override { send_cb_ = cb; }

    void FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) override {
        auto* bt = service_proto->mutable_bluetooth_service();
        bt->set_car_address("00:11:22:33:44:55");
    }

    std::vector<uint8_t> PrepareMessage(const std::vector<uint8_t>& payload) override { return payload; }

    ServiceType GetType() const override { return ServiceType::BLUETOOTH; }
    std::string GetName() const override { return "BluetoothService"; }

private:
    SendCallback send_cb_;
};

}  // namespace service
}  // namespace aauto
