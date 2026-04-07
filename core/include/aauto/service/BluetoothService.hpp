#pragma once

#include "aauto/service/ServiceBase.hpp"

namespace aauto {
namespace service {

class BluetoothService : public ServiceBase {
   public:
    explicit BluetoothService(std::string bluetooth_address);
    void FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) override;
    ServiceType GetType() const override { return ServiceType::BLUETOOTH; }
    std::string GetName() const override { return "BluetoothService"; }

   private:
    std::string bluetooth_address_;
};

} // namespace service
} // namespace aauto
