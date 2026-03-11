#define LOG_TAG "VideoService"
#include "aauto/service/VideoService.hpp"
#include "aauto/video/VideoRenderer.hpp"
#include "aauto/utils/Logger.hpp"
#include "aauto/utils/ProtocolUtil.hpp"
#include "aap_protobuf/service/media/sink/MediaSinkService.pb.h"
#include "aap_protobuf/service/media/sink/MediaMessageId.pb.h"
#include "aap_protobuf/service/media/shared/message/Setup.pb.h"
#include "aap_protobuf/service/media/shared/message/Config.pb.h"
#include "aap_protobuf/service/media/shared/message/Start.pb.h"
#include "aap_protobuf/service/media/video/message/VideoFocusRequestNotification.pb.h"
#include "aap_protobuf/service/media/video/message/VideoFocusNotification.pb.h"
#include "aap_protobuf/service/media/video/message/VideoFocusMode.pb.h"
#include "aap_protobuf/service/media/video/message/VideoFocusReason.pb.h"
#include "aap_protobuf/service/media/shared/message/MediaCodecType.pb.h"
#include "aap_protobuf/service/media/sink/message/VideoConfiguration.pb.h"
#include "aap_protobuf/service/media/sink/message/VideoCodecResolutionType.pb.h"
#include "aap_protobuf/service/media/sink/message/VideoFrameRateType.pb.h"

namespace aauto {
namespace service {

// Message type constants (from MediaMessageId.proto)
static constexpr uint16_t MSG_MEDIA_SETUP     = 0x8000;
static constexpr uint16_t MSG_MEDIA_START     = 0x8001;
static constexpr uint16_t MSG_MEDIA_STOP      = 0x8002;
static constexpr uint16_t MSG_MEDIA_CONFIG    = 0x8003;
static constexpr uint16_t MSG_MEDIA_ACK       = 0x8004;
static constexpr uint16_t MSG_VIDEO_FOCUS_REQ = 0x8007;
static constexpr uint16_t MSG_VIDEO_FOCUS_NTF = 0x8008;

void VideoService::HandleMessage(uint16_t msg_type, const std::vector<uint8_t>& payload) {
    if (msg_type == 0x07) { // ChannelOpenRequest
        HandleChannelOpenRequest(msg_type, payload, send_cb_, GetChannel());
        return;
    }

    // 비디오 스트림 데이터 (raw H264 NAL)
    // 0x00 = MEDIA_DATA (I/P 프레임), 0x01 = MEDIA_CODEC_CONFIG (SPS/PPS)
    if (msg_type == 0x00 || msg_type == 0x01) {
        if (renderer_) {
            renderer_->PushVideoData(payload);
        }
        // MediaAck - 빈 payload
        if (send_cb_) {
            send_cb_(GetChannel(), MSG_MEDIA_ACK, {});
        }
        return;
    }

    switch (msg_type) {
        case MSG_MEDIA_SETUP:
            HandleSetupRequest(payload);
            break;
        case MSG_MEDIA_START:
            HandleStartRequest(payload);
            break;
        case MSG_MEDIA_STOP:
            AA_LOG_I() << "[VideoService] MediaStopRequest 수신";
            break;
        case MSG_VIDEO_FOCUS_REQ: {
            aap_protobuf::service::media::video::message::VideoFocusRequestNotification req;
            if (req.ParseFromArray(payload.data(), payload.size())) {
                AA_LOG_I() << "[VideoService] VideoFocusRequest 수신 - mode:" << req.mode()
                           << " reason:" << req.reason();
            }
            break;
        }
        case MSG_MEDIA_ACK:
            break;
        default:
            AA_LOG_W() << "[VideoService] 미처리 msg_type: 0x" << std::hex << msg_type;
    }
}

void VideoService::HandleSetupRequest(const std::vector<uint8_t>& payload) {
    aap_protobuf::service::media::shared::message::Setup setup_req;
    if (setup_req.ParseFromArray(payload.data(), payload.size())) {
        AA_LOG_I() << "[VideoService] MediaSetupRequest 수신 - type:" << setup_req.type();
    }

    aap_protobuf::service::media::shared::message::Config config_resp;
    config_resp.set_status(aap_protobuf::service::media::shared::message::Config::STATUS_READY);
    config_resp.set_max_unacked(1);
    config_resp.add_configuration_indices(0);

    std::vector<uint8_t> out(config_resp.ByteSizeLong());
    if (config_resp.SerializeToArray(out.data(), out.size())) {
        if (send_cb_) send_cb_(GetChannel(), MSG_MEDIA_CONFIG, out);
        AA_LOG_I() << "[VideoService] ConfigResponse 송신 완료";
    }

    // VideoFocusNotification: PROJECTION focus 즉시 부여
    SendVideoFocusGain();
}

void VideoService::HandleStartRequest(const std::vector<uint8_t>& payload) {
    aap_protobuf::service::media::shared::message::Start start_req;
    if (start_req.ParseFromArray(payload.data(), payload.size())) {
        session_id_ = start_req.session_id();
        AA_LOG_I() << "[VideoService] MediaStartRequest 수신 - session_id:" << session_id_
                   << " config_index:" << start_req.configuration_index();
    }

}

void VideoService::SendVideoFocusGain() {
    aap_protobuf::service::media::video::message::VideoFocusNotification focus_ntf;
    focus_ntf.set_focus(aap_protobuf::service::media::video::message::VIDEO_FOCUS_PROJECTED);
    focus_ntf.set_unsolicited(false);

    std::vector<uint8_t> out(focus_ntf.ByteSizeLong());
    if (focus_ntf.SerializeToArray(out.data(), out.size())) {
        if (send_cb_) send_cb_(GetChannel(), MSG_VIDEO_FOCUS_NTF, out);
        AA_LOG_I() << "[VideoService] VideoFocusNotification (PROJECTION) 송신 완료";
    }
}

void VideoService::FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) {
    auto* sink = service_proto->mutable_media_sink_service();
    sink->set_available_type(aap_protobuf::service::media::shared::message::MEDIA_CODEC_VIDEO_H264_BP);

    auto* video_config = sink->add_video_configs();
    video_config->set_codec_resolution(aap_protobuf::service::media::sink::message::VIDEO_800x480);
    video_config->set_frame_rate(aap_protobuf::service::media::sink::message::VIDEO_FPS_30);
    video_config->set_width_margin(0);
    video_config->set_height_margin(0);
    video_config->set_density(140);
}

void VideoService::OnChannelOpened(uint8_t channel) {
    // 렌더러는 외부에서 SetRenderer()로 주입됨
}

}  // namespace service
}  // namespace aauto
