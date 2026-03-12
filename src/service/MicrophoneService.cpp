#define LOG_TAG "MicrophoneService"
#include "aauto/service/MicrophoneService.hpp"
#include "aauto/session/AapProtocol.hpp"
#include "aauto/utils/Logger.hpp"
#include "aap_protobuf/service/media/source/MediaSourceService.pb.h"
#include "aap_protobuf/service/media/shared/message/MediaCodecType.pb.h"
#include "aap_protobuf/service/media/shared/message/AudioConfiguration.pb.h"

namespace aauto {
namespace service {

void MicrophoneService::HandleMessage(uint16_t msg_type, const std::vector<uint8_t>& payload) {
    if (msg_type == session::aap::msg::CHANNEL_OPEN_REQUEST) {
        DispatchChannelOpen(payload);
        return;
    }

    switch (msg_type) {
        case session::aap::msg::MIC_REQUEST:
            HandleMicRequest(payload);
            break;
        default:
            AA_LOG_W() << "[MicrophoneService] 미처리 msg_type: 0x" << std::hex << msg_type;
    }
}

void MicrophoneService::HandleMicRequest(const std::vector<uint8_t>& payload) {
    if (!payload.empty()) {
        bool open = payload[0] != 0;
        AA_LOG_I() << "[MicrophoneService] MicrophoneRequest: " << (open ? "START" : "STOP");
    }
}

void MicrophoneService::FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) {
    auto* source = service_proto->mutable_media_source_service();
    source->set_available_type(aap_protobuf::service::media::shared::message::MEDIA_CODEC_AUDIO_PCM);

    auto* audio_config = source->mutable_audio_config();
    audio_config->set_sampling_rate(16000);
    audio_config->set_number_of_bits(16);
    audio_config->set_number_of_channels(1);
}

void MicrophoneService::OnChannelOpened(uint8_t channel) {}

} // namespace service
} // namespace aauto
