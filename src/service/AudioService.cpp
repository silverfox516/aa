#define LOG_TAG "AudioService"
#include "aauto/service/AudioService.hpp"
#include "aauto/session/AapProtocol.hpp"
#include "aauto/utils/Logger.hpp"
#include "aap_protobuf/service/media/sink/MediaSinkService.pb.h"
#include "aap_protobuf/service/media/shared/message/Setup.pb.h"
#include "aap_protobuf/service/media/shared/message/Config.pb.h"
#include "aap_protobuf/service/media/shared/message/Start.pb.h"
#include "aap_protobuf/service/media/shared/message/MediaCodecType.pb.h"
#include "aap_protobuf/service/media/shared/message/AudioConfiguration.pb.h"

namespace aauto {
namespace service {

AudioService::AudioService(aap_protobuf::service::media::sink::message::AudioStreamType stream_type,
                           uint32_t sample_rate, uint8_t channels, const std::string& name)
    : stream_type_(stream_type), sample_rate_(sample_rate), num_channels_(channels), name_(name) {}

void AudioService::HandleMessage(uint16_t msg_type, const std::vector<uint8_t>& payload) {
    if (msg_type == session::aap::msg::CHANNEL_OPEN_REQUEST) {
        DispatchChannelOpen(payload);
        return;
    }

    switch (msg_type) {
        case session::aap::msg::MEDIA_DATA:
            break;
        case session::aap::msg::MEDIA_SETUP:
            HandleSetupRequest(payload);
            break;
        case session::aap::msg::MEDIA_START: {
            aap_protobuf::service::media::shared::message::Start start_req;
            if (start_req.ParseFromArray(payload.data(), payload.size())) {
                AA_LOG_I() << "[" << name_ << "] MediaStartRequest - session_id:"
                           << start_req.session_id();
            }
            break;
        }
        case session::aap::msg::MEDIA_STOP:
            AA_LOG_I() << "[" << name_ << "] MediaStopRequest";
            break;
        case session::aap::msg::MEDIA_ACK:
            break;
        default:
            AA_LOG_W() << "[" << name_ << "] 미처리 msg_type: 0x" << std::hex << msg_type;
    }
}

void AudioService::HandleSetupRequest(const std::vector<uint8_t>& payload) {
    aap_protobuf::service::media::shared::message::Setup setup_req;
    if (setup_req.ParseFromArray(payload.data(), payload.size())) {
        AA_LOG_I() << "[" << name_ << "] MediaSetupRequest - type:" << setup_req.type();
    }

    aap_protobuf::service::media::shared::message::Config config_resp;
    config_resp.set_status(aap_protobuf::service::media::shared::message::Config::STATUS_READY);
    config_resp.set_max_unacked(1);
    config_resp.add_configuration_indices(0);

    std::vector<uint8_t> out(config_resp.ByteSizeLong());
    if (config_resp.SerializeToArray(out.data(), out.size())) {
        if (send_cb_) send_cb_(channel_, session::aap::msg::MEDIA_CONFIG, out);
        AA_LOG_I() << "[" << name_ << "] ConfigResponse 송신 완료";
    }
}

void AudioService::FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) {
    auto* sink = service_proto->mutable_media_sink_service();
    sink->set_available_type(aap_protobuf::service::media::shared::message::MEDIA_CODEC_AUDIO_PCM);
    sink->set_audio_type(stream_type_);

    auto* audio_config = sink->add_audio_configs();
    audio_config->set_sampling_rate(sample_rate_);
    audio_config->set_number_of_bits(16);
    audio_config->set_number_of_channels(num_channels_);
}

void AudioService::OnChannelOpened(uint8_t channel) {}

} // namespace service
} // namespace aauto
