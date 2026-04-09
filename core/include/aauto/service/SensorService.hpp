#pragma once

#include "aauto/service/ServiceBase.hpp"

namespace aauto {
namespace service {

// Per-sensor enable flags. Each flag controls both the advertisement in
// FillServiceDefinition AND the actual data path (OnChannelOpened / start
// request). Adding a sensor type means adding a flag here, an entry in
// FillServiceDefinition, and a send path. Advertisement and send live in
// the same class so they cannot drift out of sync.
struct SensorServiceConfig {
    bool driving_status = true;
    bool night_mode     = false;
    bool location       = false;
};

class SensorService : public ServiceBase {
   public:
    explicit SensorService(SensorServiceConfig config);
    void FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) override;
    void OnChannelOpened(uint8_t channel) override;
    ServiceType GetType() const override { return ServiceType::SENSOR; }
    std::string GetName() const override { return "SensorService"; }

   private:
    void HandleSensorStartRequest(const std::vector<uint8_t>& payload);
    void SendDrivingStatus();

    SensorServiceConfig config_;
};

} // namespace service
} // namespace aauto
