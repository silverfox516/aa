#pragma once

#include "aauto/service/ServiceBase.hpp"
#include "aap_protobuf/service/media/sink/message/AudioStreamType.pb.h"

namespace aauto {
namespace service {

class AudioService : public ServiceBase {
   public:
    AudioService(aap_protobuf::service::media::sink::message::AudioStreamType stream_type,
                 uint32_t sample_rate, uint8_t channels, const std::string& name);

    void FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) override;
    void OnChannelOpened(uint8_t channel) override;
    ServiceType GetType() const override { return ServiceType::AUDIO; }
    std::string GetName() const override { return name_; }

   private:
    void HandleSetupRequest(const std::vector<uint8_t>& payload);

    aap_protobuf::service::media::sink::message::AudioStreamType stream_type_;
    uint32_t sample_rate_;
    uint8_t num_channels_;
    std::string name_;
};

} // namespace service
} // namespace aauto
