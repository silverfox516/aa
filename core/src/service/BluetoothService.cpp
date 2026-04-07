#define LOG_TAG "AA.CORE.BluetoothService"
#include "aauto/service/BluetoothService.hpp"
#include "aauto/session/AapProtocol.hpp"

namespace aauto {
namespace service {

BluetoothService::BluetoothService(std::string bluetooth_address)
    : bluetooth_address_(std::move(bluetooth_address)) {}

void BluetoothService::FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) {
    auto* bt = service_proto->mutable_bluetooth_service();
    bt->set_car_address(bluetooth_address_);
}

} // namespace service
} // namespace aauto
