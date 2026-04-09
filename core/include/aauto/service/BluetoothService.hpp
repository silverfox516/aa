#pragma once

#include <string>

#include "aauto/service/ServiceBase.hpp"

namespace aauto {
namespace service {

// Options that the app layer must supply to enable the Bluetooth service.
struct BluetoothServiceConfig {
    std::string car_address;
};

class BluetoothService : public ServiceBase {
   public:
    explicit BluetoothService(BluetoothServiceConfig config);
    void FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) override;
    ServiceType GetType() const override { return ServiceType::BLUETOOTH; }
    std::string GetName() const override { return "BluetoothService"; }

   private:
    BluetoothServiceConfig config_;
};

} // namespace service
} // namespace aauto
