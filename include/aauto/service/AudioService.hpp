#pragma once

#include "aauto/service/IService.hpp"
#include "aap_protobuf/service/media/sink/message/AudioStreamType.pb.h"

namespace aauto {
namespace service {

class AudioService : public IService {
   public:
    AudioService(aap_protobuf::service::media::sink::message::AudioStreamType stream_type,
                 uint32_t sample_rate, uint8_t channels, const std::string& name);

    void HandleMessage(uint16_t msg_type, const std::vector<uint8_t>& payload) override;
    void SetSendCallback(SendCallback cb) override { send_cb_ = cb; }
    void FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) override;
    void OnChannelOpened(uint8_t channel) override;
    std::vector<uint8_t> PrepareMessage(const std::vector<uint8_t>& payload) override { return payload; }
    ServiceType GetType() const override { return ServiceType::AUDIO; }
    std::string GetName() const override { return name_; }

   private:
    void HandleSetupRequest(const std::vector<uint8_t>& payload);

    SendCallback send_cb_;
    aap_protobuf::service::media::sink::message::AudioStreamType stream_type_;
    uint32_t sample_rate_;
    uint8_t num_channels_;
    std::string name_;
};

}  // namespace service
}  // namespace aauto
