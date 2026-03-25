#pragma once

#include "aauto/service/ServiceBase.hpp"

namespace aauto {
namespace service {

class BluetoothService : public ServiceBase {
   public:
    void FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) override;
    ServiceType GetType() const override { return ServiceType::BLUETOOTH; }
    std::string GetName() const override { return "BluetoothService"; }
};

} // namespace service
} // namespace aauto
