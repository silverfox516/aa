#define LOG_TAG "MicrophoneService"
#include "aauto/service/MicrophoneService.hpp"
#include "aauto/utils/Logger.hpp"
#include "aauto/utils/ProtocolUtil.hpp"
#include "aap_protobuf/service/media/source/MediaSourceService.pb.h"
#include "aap_protobuf/service/media/sink/MediaMessageId.pb.h"
#include "aap_protobuf/service/media/shared/message/Setup.pb.h"
#include "aap_protobuf/service/media/shared/message/Config.pb.h"
#include "aap_protobuf/service/media/shared/message/MediaCodecType.pb.h"
#include "aap_protobuf/service/media/shared/message/AudioConfiguration.pb.h"

namespace aauto {
namespace service {

// Mic uses same media message IDs
static constexpr uint16_t MSG_MIC_REQUEST  = 0x800A; // MEDIA_MESSAGE_MICROPHONE_REQUEST
static constexpr uint16_t MSG_MIC_RESPONSE = 0x800B; // MEDIA_MESSAGE_MICROPHONE_RESPONSE

void MicrophoneService::HandleMessage(uint16_t msg_type, const std::vector<uint8_t>& payload) {
    if (msg_type == 0x07) { // ChannelOpenRequest
        HandleChannelOpenRequest(msg_type, payload, send_cb_, GetChannel());
        return;
    }

    switch (msg_type) {
        case MSG_MIC_REQUEST:
            HandleMicRequest(payload);
            break;
        default:
            AA_LOG_W() << "[MicrophoneService] 미처리 msg_type: 0x" << std::hex << msg_type;
    }
}

void MicrophoneService::HandleMicRequest(const std::vector<uint8_t>& payload) {
    // MicrophoneRequest: open=true 면 녹음 시작, false 면 중지
    // payload: 첫 바이트 파싱 (단순 bool 필드)
    if (!payload.empty()) {
        bool open = payload[0] != 0;
        AA_LOG_I() << "[MicrophoneService] MicrophoneRequest: " << (open ? "START" : "STOP");
        // TODO: 실제 마이크 녹음 구현 연결
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

void MicrophoneService::OnChannelOpened(uint8_t channel) {
    // TODO: 마이크 장치 초기화
}

}  // namespace service
}  // namespace aauto
