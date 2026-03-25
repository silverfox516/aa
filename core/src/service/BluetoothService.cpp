#define LOG_TAG "BluetoothService"
#include "aauto/service/BluetoothService.hpp"
#include "aauto/session/AapProtocol.hpp"

namespace aauto {
namespace service {

// No additional message handlers — only CHANNEL_OPEN_REQUEST, handled by ServiceBase.

void BluetoothService::FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) {
    auto* bt = service_proto->mutable_bluetooth_service();
    bt->set_car_address("00:11:22:33:44:55");
}

} // namespace service
} // namespace aauto
