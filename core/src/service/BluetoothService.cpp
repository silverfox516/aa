#define LOG_TAG "AA.CORE.BluetoothService"
#include "aauto/service/BluetoothService.hpp"
#include "aauto/session/AapProtocol.hpp"

namespace aauto {
namespace service {

BluetoothService::BluetoothService(BluetoothServiceConfig config)
    : config_(std::move(config)) {}

void BluetoothService::FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) {
    auto* bt = service_proto->mutable_bluetooth_service();
    bt->set_car_address(config_.car_address);
}

} // namespace service
} // namespace aauto
