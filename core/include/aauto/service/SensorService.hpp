#pragma once

#include <cstdint>

#include "aauto/service/ServiceBase.hpp"

namespace aauto {
namespace service {

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
