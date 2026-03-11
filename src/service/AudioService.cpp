#define LOG_TAG "AudioService"
#include "aauto/service/AudioService.hpp"
#include "aauto/utils/Logger.hpp"
#include "aauto/utils/ProtocolUtil.hpp"
#include "aap_protobuf/service/media/sink/MediaSinkService.pb.h"
#include "aap_protobuf/service/media/sink/MediaMessageId.pb.h"
#include "aap_protobuf/service/media/shared/message/Setup.pb.h"
#include "aap_protobuf/service/media/shared/message/Config.pb.h"
#include "aap_protobuf/service/media/shared/message/Start.pb.h"
#include "aap_protobuf/service/media/shared/message/MediaCodecType.pb.h"
#include "aap_protobuf/service/media/sink/message/AudioStreamType.pb.h"
#include "aap_protobuf/service/media/shared/message/AudioConfiguration.pb.h"

namespace aauto {
namespace service {

static constexpr uint16_t MSG_MEDIA_SETUP   = 0x8000;
static constexpr uint16_t MSG_MEDIA_START   = 0x8001;
static constexpr uint16_t MSG_MEDIA_STOP    = 0x8002;
static constexpr uint16_t MSG_MEDIA_CONFIG  = 0x8003;
static constexpr uint16_t MSG_MEDIA_ACK     = 0x8004;

AudioService::AudioService(aap_protobuf::service::media::sink::message::AudioStreamType stream_type,
                           uint32_t sample_rate, uint8_t channels, const std::string& name)
    : stream_type_(stream_type), sample_rate_(sample_rate), num_channels_(channels), name_(name) {}

void AudioService::HandleMessage(uint16_t msg_type, const std::vector<uint8_t>& payload) {
    if (msg_type == 0x07) { // ChannelOpenRequest
        HandleChannelOpenRequest(msg_type, payload, send_cb_, channel_);
        return;
    }

    switch (msg_type) {
        case 0x00:
            // raw audio data - 현재는 로그만 남기거나 무시 (나중에 디코딩/재생 추가 가능)
            // AA_LOG_D() << "[" << name_ << "] Audio 데이터 수신 (" << payload.size() << " bytes)";
            break;
        case MSG_MEDIA_SETUP:
            HandleSetupRequest(payload);
            break;
        case MSG_MEDIA_START: {
            aap_protobuf::service::media::shared::message::Start start_req;
            if (start_req.ParseFromArray(payload.data(), payload.size())) {
                AA_LOG_I() << "[" << name_ << "] MediaStartRequest - session_id:"
                           << start_req.session_id();
            }
            break;
        }
        case MSG_MEDIA_STOP:
            AA_LOG_I() << "[" << name_ << "] MediaStopRequest";
            break;
        case MSG_MEDIA_ACK:
            // 미디어 ACK 무시 (폰이 우리 ConfigResponse를 확인했다는 알림)
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

    // ConfigResponse: STATUS_READY, max_unacked=1, configuration_index=0
    aap_protobuf::service::media::shared::message::Config config_resp;
    config_resp.set_status(aap_protobuf::service::media::shared::message::Config::STATUS_READY);
    config_resp.set_max_unacked(1);
    config_resp.add_configuration_indices(0);

    std::vector<uint8_t> out(config_resp.ByteSizeLong());
    if (config_resp.SerializeToArray(out.data(), out.size())) {
        if (send_cb_) send_cb_(channel_, MSG_MEDIA_CONFIG, out);
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

void AudioService::OnChannelOpened(uint8_t channel) {
    // TODO: 오디오 출력 장치 초기화
}

}  // namespace service
}  // namespace aauto
