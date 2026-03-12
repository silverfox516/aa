#define LOG_TAG "BluetoothService"
#include "aauto/service/BluetoothService.hpp"
#include "aauto/session/AapProtocol.hpp"

namespace aauto {
namespace service {

void BluetoothService::HandleMessage(uint16_t msg_type, const std::vector<uint8_t>& payload) {
    if (msg_type == session::aap::msg::CHANNEL_OPEN_REQUEST) {
        DispatchChannelOpen(payload);
    }
}

void BluetoothService::FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) {
    auto* bt = service_proto->mutable_bluetooth_service();
    bt->set_car_address("00:11:22:33:44:55");
}

} // namespace service
} // namespace aauto
