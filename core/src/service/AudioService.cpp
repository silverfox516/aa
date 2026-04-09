#define LOG_TAG "AA.CORE.AudioService"
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

#include <cstring>

namespace aauto {
namespace service {

namespace msg = session::aap::msg;

namespace {
// AAP audio MEDIA_DATA payload is prefixed by an 8-byte int64 timestamp.
constexpr size_t kAudioTimestampBytes = 8;
}

AudioService::AudioService(AudioServiceConfig config)
    : config_(std::move(config)) {
    cached_format_.sample_rate     = config_.sample_rate;
    cached_format_.channel_count   = config_.channels;
    cached_format_.bits_per_sample = config_.bits_per_sample;

    RegisterHandler(msg::MEDIA_DATA, [this](const std::vector<uint8_t>& p) {
        ++media_data_count_;
        if (media_data_count_ <= 10 || media_data_count_ % 100 == 0) {
            AA_LOG_I() << "[" << config_.name << "] MediaData #" << media_data_count_
                       << " size=" << p.size() << " session_id=" << session_id_;
        }

        // ACK first so the phone immediately transmits the next packet.
        aap_protobuf::service::media::source::message::Ack ack;
        ack.set_session_id(session_id_);
        ack.set_ack(1);
        std::vector<uint8_t> out(ack.ByteSize());
        if (ack.SerializeToArray(out.data(), out.size())) {
            if (send_cb_) send_cb_(channel_, msg::MEDIA_ACK, out);
        }

        if (p.size() <= kAudioTimestampBytes) return;

        uint64_t pts_us = 0;
        std::memcpy(&pts_us, p.data(), sizeof(pts_us));

        std::shared_ptr<IAudioSink> sink_copy;
        {
            std::lock_guard<std::mutex> lock(sink_mutex_);
            sink_copy = sink_;
        }
        if (sink_copy) {
            sink_copy->OnAudioData(p.data() + kAudioTimestampBytes,
                                   p.size() - kAudioTimestampBytes,
                                   pts_us);
        }
    });
    RegisterHandler(msg::MEDIA_SETUP, [this](const auto& p) { HandleSetupRequest(p); });
    RegisterHandler(msg::MEDIA_START, [this](const auto& p) {
        aap_protobuf::service::media::shared::message::Start start_req;
        if (start_req.ParseFromArray(p.data(), p.size())) {
            session_id_ = start_req.session_id();
            AA_LOG_I() << "[" << config_.name << "] MediaStartRequest - session_id:" << session_id_;
        }
        media_data_count_ = 0;
        // Decoder/AudioTrack lifecycle is owned by the sink.
    });
    RegisterHandler(msg::MEDIA_STOP, [this](const auto&) {
        AA_LOG_I() << "[" << config_.name << "] MediaStopRequest after " << media_data_count_ << " frames";
        // No action: sink lifetime is owned by the app layer.
    });
    RegisterHandler(msg::MEDIA_ACK, [](const auto&) {});
}

void AudioService::SetSink(std::shared_ptr<IAudioSink> sink) {
    std::shared_ptr<IAudioSink> old;
    std::shared_ptr<IAudioSink> to_replay;
    AudioFormat                 fmt_copy;
    {
        std::lock_guard<std::mutex> lock(sink_mutex_);
        old   = std::move(sink_);
        sink_ = sink;
        if (sink_) {
            to_replay = sink_;
            fmt_copy  = cached_format_;
        }
    }
    old.reset();

    if (to_replay) {
        AA_LOG_I() << "[" << config_.name << "] SetSink: replaying format "
                   << fmt_copy.sample_rate << "Hz/" << int(fmt_copy.channel_count) << "ch";
        to_replay->OnAudioFormat(fmt_copy);
    }
}

void AudioService::HandleSetupRequest(const std::vector<uint8_t>& payload) {
    aap_protobuf::service::media::shared::message::Setup setup_req;
    if (setup_req.ParseFromArray(payload.data(), payload.size())) {
        AA_LOG_I() << "[" << config_.name << "] MediaSetupRequest - type:" << setup_req.type();
    }

    aap_protobuf::service::media::shared::message::Config config_resp;
    config_resp.set_status(aap_protobuf::service::media::shared::message::Config::STATUS_READY);
    config_resp.set_max_unacked(4);
    config_resp.add_configuration_indices(0);

    std::vector<uint8_t> out(config_resp.ByteSize());
    if (config_resp.SerializeToArray(out.data(), out.size())) {
        if (send_cb_) send_cb_(channel_, msg::MEDIA_CONFIG, out);
        AA_LOG_I() << "[" << config_.name << "] ConfigResponse sent";
    }
}

void AudioService::FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) {
    auto* sink = service_proto->mutable_media_sink_service();
    sink->set_available_type(aap_protobuf::service::media::shared::message::MEDIA_CODEC_AUDIO_PCM);
    sink->set_audio_type(config_.stream_type);

    auto* audio_config = sink->add_audio_configs();
    audio_config->set_sampling_rate(config_.sample_rate);
    audio_config->set_number_of_bits(config_.bits_per_sample);
    audio_config->set_number_of_channels(config_.channels);
}

void AudioService::OnChannelOpened(uint8_t) {}

void AudioService::OnSessionStopped() {
    std::shared_ptr<IAudioSink> dead;
    {
        std::lock_guard<std::mutex> lock(sink_mutex_);
        dead = std::move(sink_);
    }
    dead.reset();
}

} // namespace service
} // namespace aauto
