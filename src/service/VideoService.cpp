#define LOG_TAG "VideoService"
#include "aauto/service/VideoService.hpp"
#include "aauto/session/AapProtocol.hpp"
#include "aauto/utils/Logger.hpp"
#include "aap_protobuf/service/media/sink/MediaSinkService.pb.h"
#include "aap_protobuf/service/media/shared/message/Setup.pb.h"
#include "aap_protobuf/service/media/shared/message/Config.pb.h"
#include "aap_protobuf/service/media/shared/message/Start.pb.h"
#include "aap_protobuf/service/media/shared/message/MediaCodecType.pb.h"
#include "aap_protobuf/service/media/sink/message/VideoConfiguration.pb.h"
#include "aap_protobuf/service/media/sink/message/VideoCodecResolutionType.pb.h"
#include "aap_protobuf/service/media/sink/message/VideoFrameRateType.pb.h"
#include "aap_protobuf/service/media/video/message/VideoFocusRequestNotification.pb.h"
#include "aap_protobuf/service/media/video/message/VideoFocusNotification.pb.h"
#include "aap_protobuf/service/media/video/message/VideoFocusMode.pb.h"
#include "aap_protobuf/service/media/source/message/Ack.pb.h"

namespace aauto {
namespace service {

namespace msg = session::aap::msg;

VideoService::VideoService(core::HeadunitConfig config,
                           std::shared_ptr<platform::IVideoOutput> video_output)
    : config_(std::move(config)), video_output_(std::move(video_output)) {}

void VideoService::HandleMessage(uint16_t msg_type, const std::vector<uint8_t>& payload) {
    if (msg_type == msg::CHANNEL_OPEN_REQUEST) {
        DispatchChannelOpen(payload);
        return;
    }

    if (msg_type == msg::MEDIA_DATA || msg_type == msg::MEDIA_CODEC_CONFIG) {
        if (video_output_) video_output_->PushVideoData(payload);
        SendMediaAck();
        return;
    }

    switch (msg_type) {
        case msg::MEDIA_SETUP:
            HandleSetupRequest(payload);
            break;
        case msg::MEDIA_START:
            HandleStartRequest(payload);
            break;
        case msg::MEDIA_STOP:
            AA_LOG_I() << "[VideoService] MediaStopRequest 수신";
            if (video_output_) video_output_->Close();
            break;
        case msg::VIDEO_FOCUS_REQUEST: {
            aap_protobuf::service::media::video::message::VideoFocusRequestNotification req;
            if (req.ParseFromArray(payload.data(), payload.size())) {
                AA_LOG_I() << "[VideoService] VideoFocusRequest - mode:" << req.mode()
                           << " reason:" << req.reason();
            }
            break;
        }
        case msg::MEDIA_ACK:
            break;
        default:
            AA_LOG_W() << "[VideoService] 미처리 msg_type: 0x" << std::hex << msg_type;
    }
}

void VideoService::HandleSetupRequest(const std::vector<uint8_t>& payload) {
    aap_protobuf::service::media::shared::message::Setup setup_req;
    if (setup_req.ParseFromArray(payload.data(), payload.size())) {
        AA_LOG_I() << "[VideoService] MediaSetupRequest - type:" << setup_req.type();
    }

    aap_protobuf::service::media::shared::message::Config config_resp;
    config_resp.set_status(aap_protobuf::service::media::shared::message::Config::STATUS_READY);
    config_resp.set_max_unacked(10);
    config_resp.add_configuration_indices(0);

    std::vector<uint8_t> out(config_resp.ByteSizeLong());
    if (config_resp.SerializeToArray(out.data(), out.size())) {
        if (send_cb_) send_cb_(GetChannel(), msg::MEDIA_CONFIG, out);
        AA_LOG_I() << "[VideoService] ConfigResponse 송신 완료";
    }

    SendVideoFocusGain();
}

void VideoService::HandleStartRequest(const std::vector<uint8_t>& payload) {
    aap_protobuf::service::media::shared::message::Start start_req;
    if (start_req.ParseFromArray(payload.data(), payload.size())) {
        session_id_ = start_req.session_id();
        AA_LOG_I() << "[VideoService] MediaStartRequest - session_id:" << session_id_
                   << " config_index:" << start_req.configuration_index();
    }
    if (video_output_) video_output_->Open(config_.display_width, config_.display_height);
}

void VideoService::SendVideoFocusGain() {
    aap_protobuf::service::media::video::message::VideoFocusNotification focus_ntf;
    focus_ntf.set_focus(aap_protobuf::service::media::video::message::VIDEO_FOCUS_PROJECTED);
    focus_ntf.set_unsolicited(false);

    std::vector<uint8_t> out(focus_ntf.ByteSizeLong());
    if (focus_ntf.SerializeToArray(out.data(), out.size())) {
        if (send_cb_) send_cb_(GetChannel(), msg::VIDEO_FOCUS_NOTIFICATION, out);
        AA_LOG_I() << "[VideoService] VideoFocusNotification(PROJECTION) 송신 완료";
    }
}

void VideoService::SendMediaAck() {
    aap_protobuf::service::media::source::message::Ack ack;
    ack.set_session_id(session_id_);
    ack.set_ack(1);

    std::vector<uint8_t> out(ack.ByteSizeLong());
    if (ack.SerializeToArray(out.data(), out.size())) {
        if (send_cb_) send_cb_(GetChannel(), msg::MEDIA_ACK, out);
    }
}

void VideoService::FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) {
    auto* sink = service_proto->mutable_media_sink_service();
    sink->set_available_type(aap_protobuf::service::media::shared::message::MEDIA_CODEC_VIDEO_H264_BP);

    auto* video_config = sink->add_video_configs();
    video_config->set_codec_resolution(aap_protobuf::service::media::sink::message::VIDEO_1280x720);
    video_config->set_frame_rate(aap_protobuf::service::media::sink::message::VIDEO_FPS_30);
    video_config->set_width_margin(0);
    video_config->set_height_margin(0);
    video_config->set_density(config_.display_density);
}

void VideoService::OnChannelOpened(uint8_t channel) {}

void VideoService::OnSessionStopped() {
    if (video_output_) video_output_->Close();
}

} // namespace service
} // namespace aauto
