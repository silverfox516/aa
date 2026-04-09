#pragma once

#include <cstdint>

#include "aauto/service/ServiceBase.hpp"

namespace aauto {
namespace service {

// Options that the app layer must supply to enable the microphone source service.
struct MicrophoneServiceConfig {
    uint32_t sample_rate     = 16000;
    uint8_t  channels        = 1;
    uint8_t  bits_per_sample = 16;
};

class MicrophoneService : public ServiceBase {
   public:
    explicit MicrophoneService(MicrophoneServiceConfig config);
    void FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) override;
    void OnChannelOpened(uint8_t channel) override;
    ServiceType GetType() const override { return ServiceType::MIC; }
    std::string GetName() const override { return "MicrophoneService"; }

   private:
    void HandleMicRequest(const std::vector<uint8_t>& payload);

    MicrophoneServiceConfig config_;
};

} // namespace service
} // namespace aauto
