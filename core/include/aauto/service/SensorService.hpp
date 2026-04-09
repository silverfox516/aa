#pragma once

#include <cstdint>

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

    // Push one location fix from the app layer (LocationManager listener).
    // No-op when config_.location is false. Optional fields may be passed
    // as the sentinels INT32_MIN / 0 if the platform did not report them.
    // timestamp_us is unix microseconds; pass 0 to use the current time.
    void SendLocationFix(int32_t lat_e7, int32_t lon_e7,
                          int32_t alt_e2     = INT32_MIN,
                          uint32_t accuracy_e3 = 0,
                          int32_t speed_e3   = INT32_MIN,
                          int32_t bearing_e6 = INT32_MIN,
                          uint64_t timestamp_us = 0);

   private:
    void HandleSensorStartRequest(const std::vector<uint8_t>& payload);
    void SendDrivingStatus();

    SensorServiceConfig config_;
};

} // namespace service
} // namespace aauto
