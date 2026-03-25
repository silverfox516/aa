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
#include "aap_protobuf/service/media/source/message/Ack.pb.h"

namespace aauto {
namespace service {

AudioService::AudioService(aap_protobuf::service::media::sink::message::AudioStreamType stream_type,
                           uint32_t sample_rate, uint8_t channels, const std::string& name,
                           std::shared_ptr<platform::IAudioOutput> audio_output)
    : stream_type_(stream_type), sample_rate_(sample_rate), num_channels_(channels)
    , name_(name), audio_output_(std::move(audio_output)) {

    namespace msg = session::aap::msg;
    RegisterHandler(msg::MEDIA_DATA, [this](const auto& p) {
        if (audio_output_) audio_output_->PushAudioData(p);
        // ACK를 보내야 폰이 다음 패킷을 즉시 전송함
        aap_protobuf::service::media::source::message::Ack ack;
        ack.set_session_id(session_id_);
        ack.set_ack(1);
        std::vector<uint8_t> out(ack.ByteSizeLong());
        if (ack.SerializeToArray(out.data(), out.size())) {
            if (send_cb_) send_cb_(channel_, msg::MEDIA_ACK, out);
        }
    });
    RegisterHandler(msg::MEDIA_SETUP, [this](const auto& p){ HandleSetupRequest(p); });
    RegisterHandler(msg::MEDIA_START, [this](const auto& p) {
        aap_protobuf::service::media::shared::message::Start start_req;
        if (start_req.ParseFromArray(p.data(), p.size())) {
            session_id_ = start_req.session_id();
            AA_LOG_I() << "[" << name_ << "] MediaStartRequest - session_id:" << session_id_;
        }
        if (audio_output_) audio_output_->Open(sample_rate_, num_channels_, 16);
    });
    RegisterHandler(msg::MEDIA_STOP, [this](const auto&) {
        AA_LOG_I() << "[" << name_ << "] MediaStopRequest";
        if (audio_output_) audio_output_->Close();
    });
    RegisterHandler(msg::MEDIA_ACK, [](const auto&){});
}

void AudioService::HandleSetupRequest(const std::vector<uint8_t>& payload) {
    aap_protobuf::service::media::shared::message::Setup setup_req;
    if (setup_req.ParseFromArray(payload.data(), payload.size())) {
        AA_LOG_I() << "[" << name_ << "] MediaSetupRequest - type:" << setup_req.type();
    }

    aap_protobuf::service::media::shared::message::Config config_resp;
    config_resp.set_status(aap_protobuf::service::media::shared::message::Config::STATUS_READY);
    config_resp.set_max_unacked(4);
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

void AudioService::OnChannelOpened(uint8_t) {}

void AudioService::OnSessionStopped() {
    if (audio_output_) audio_output_->Close();
}

} // namespace service
} // namespace aauto
