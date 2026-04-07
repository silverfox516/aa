#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "aauto/service/AudioSink.hpp"
#include "aauto/service/ServiceBase.hpp"
#include "aap_protobuf/service/media/sink/message/AudioStreamType.pb.h"

namespace aauto {
namespace service {

// One AudioService instance == one AAP audio channel (media / guidance / system).
// Pure data source: it owns no platform output. The consumer attaches an
// IAudioSink to receive PCM data. While no sink is attached, frames are
// dropped.
//
// Audio focus is a session-wide concept managed by ControlService and
// orchestrated by the app layer; this class only gates data flow.
class AudioService : public ServiceBase {
   public:
    AudioService(aap_protobuf::service::media::sink::message::AudioStreamType stream_type,
                 uint32_t sample_rate, uint8_t channels, const std::string& name);

    void FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) override;
    void OnChannelOpened(uint8_t channel) override;
    void OnSessionStopped() override;
    ServiceType GetType() const override { return ServiceType::AUDIO; }
    std::string GetName() const override { return name_; }

    // Attach or detach a sink. Thread-safe. Pass nullptr to detach.
    // Attaching immediately replays the cached AudioFormat.
    void SetSink(std::shared_ptr<IAudioSink> sink);

   private:
    void HandleSetupRequest(const std::vector<uint8_t>& payload);

    aap_protobuf::service::media::sink::message::AudioStreamType stream_type_;
    uint32_t    sample_rate_;
    uint8_t     num_channels_;
    std::string name_;
    int32_t     session_id_ = 0;
    uint64_t    media_data_count_ = 0;

    AudioFormat                 cached_format_;
    std::mutex                  sink_mutex_;
    std::shared_ptr<IAudioSink> sink_;
};

} // namespace service
} // namespace aauto
