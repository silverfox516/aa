#define LOG_TAG "AA.CORE.VideoService"
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

#include <cstring>

namespace aauto {
namespace service {

namespace msg = session::aap::msg;

namespace {
// AAP video MEDIA_DATA payload is prefixed by an 8-byte int64 timestamp
// (microseconds), matching the audio path. The remaining bytes are the
// H.264 byte stream.
constexpr size_t kVideoTimestampBytes = 8;
}

VideoService::VideoService(VideoServiceConfig config)
    : config_(std::move(config)) {
    // Dimensions/fps are known at construction time — they match what
    // we advertise in FillServiceDefinition. codec_data is filled in
    // when the phone sends MEDIA_CODEC_CONFIG.
    cached_config_.width  = config_.width;
    cached_config_.height = config_.height;
    cached_config_.fps    = config_.fps;

    RegisterHandler(msg::MEDIA_CODEC_CONFIG, [this](const auto& p) { HandleCodecConfig(p); });
    RegisterHandler(msg::MEDIA_DATA,         [this](const auto& p) { HandleMediaData(p); });
    RegisterHandler(msg::MEDIA_SETUP,        [this](const auto& p) { HandleSetupRequest(p); });
    RegisterHandler(msg::MEDIA_START,        [this](const auto& p) { HandleStartRequest(p); });
    RegisterHandler(msg::MEDIA_STOP,         [](const auto&) {
        AA_LOG_I() << "[VideoService] MediaStopRequest received";
        // No action: sink lifetime is owned by the app layer, not by the
        // phone-side stream stop. A subsequent MEDIA_START will resume
        // pushing frames into the same sink.
    });
    RegisterHandler(msg::VIDEO_FOCUS_REQUEST, [](const auto& p) {
        aap_protobuf::service::media::video::message::VideoFocusRequestNotification req;
        if (req.ParseFromArray(p.data(), p.size())) {
            AA_LOG_I() << "[VideoService] VideoFocusRequest - mode:" << req.mode()
                       << " reason:" << req.reason();
        }
    });
    RegisterHandler(msg::MEDIA_ACK, [](const auto&) {});
}

void VideoService::SetSink(std::shared_ptr<IVideoSink> sink) {
    // Hold sink_mutex_ across the entire swap + replay so that:
    //   1. A concurrent HandleCodecConfig cannot start a new OnVideoConfig
    //      on the old sink while we are switching.
    //   2. The previous sink is fully destroyed (its decoder torn down,
    //      its surface released) before the new sink's OnVideoConfig
    //      tries to configure a decoder on the same surface.
    //
    // Cost: this call may take ~20ms (decoder setup). HandleMediaData
    // briefly contends on the same mutex; one or two frames may be
    // delayed during a sink switch, which is acceptable.
    std::lock_guard<std::mutex> lock(sink_mutex_);

    auto old = std::move(sink_);
    sink_    = sink;
    old.reset();  // run old sink dtor (codec stop, surface release) here

    if (sink_ && have_codec_data_) {
        AA_LOG_I() << "[VideoService] SetSink: replaying cached codec config "
                   << cached_config_.width << "x" << cached_config_.height
                   << " codec_data=" << cached_config_.codec_data.size() << "B";
        sink_->OnVideoConfig(cached_config_);
    }

    if (sink_) {
        SendVideoFocusGain();
    } else {
        SendVideoFocusLoss();
    }
}

void VideoService::HandleCodecConfig(const std::vector<uint8_t>& payload) {
    AA_LOG_I() << "[VideoService] CodecConfig size=" << payload.size();

    {
        std::lock_guard<std::mutex> lock(sink_mutex_);
        cached_config_.codec_data = payload;
        have_codec_data_          = true;
        if (sink_) {
            // Call under the lock to serialize with SetSink. The sink is
            // owned by sink_ for the duration of the call, so it cannot
            // be destroyed mid-callback.
            sink_->OnVideoConfig(cached_config_);
        }
    }
    SendMediaAck();
}

void VideoService::HandleMediaData(const std::vector<uint8_t>& payload) {
    ++video_frame_count_;
    if (video_frame_count_ <= 10 || video_frame_count_ % 100 == 0) {
        AA_LOG_I() << "[VideoService] MediaData frame=" << video_frame_count_
                   << " size=" << payload.size();
    }

    if (payload.size() > kVideoTimestampBytes) {
        uint64_t pts_us = 0;
        std::memcpy(&pts_us, payload.data(), sizeof(pts_us));

        std::shared_ptr<IVideoSink> sink_copy;
        {
            std::lock_guard<std::mutex> lock(sink_mutex_);
            sink_copy = sink_;
        }
        if (sink_copy) {
            VideoFrame frame{
                payload.data() + kVideoTimestampBytes,
                payload.size() - kVideoTimestampBytes,
                pts_us,
                /*is_keyframe=*/false,
            };
            sink_copy->OnVideoFrame(frame);
        }
    }
    // Always ack to keep flow control going, even if we dropped the frame.
    SendMediaAck();
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

    std::vector<uint8_t> out(config_resp.ByteSize());
    if (config_resp.SerializeToArray(out.data(), out.size())) {
        if (send_cb_) send_cb_(GetChannel(), msg::MEDIA_CONFIG, out);
        AA_LOG_I() << "[VideoService] ConfigResponse sent";
    }

    // Default state on a fresh setup is "no focus" — the phone holds off
    // streaming until SetSink(non-null) is called by the app layer.
    SendVideoFocusLoss();
}

void VideoService::HandleStartRequest(const std::vector<uint8_t>& payload) {
    aap_protobuf::service::media::shared::message::Start start_req;
    if (start_req.ParseFromArray(payload.data(), payload.size())) {
        session_id_ = start_req.session_id();
        AA_LOG_I() << "[VideoService] MediaStartRequest - session_id:" << session_id_
                   << " config_index:" << start_req.configuration_index();
    }
    // Decoder lifecycle is owned by the sink. Nothing to do here.
}

void VideoService::SendVideoFocusGain() {
    aap_protobuf::service::media::video::message::VideoFocusNotification ntf;
    ntf.set_focus(aap_protobuf::service::media::video::message::VIDEO_FOCUS_PROJECTED);
    ntf.set_unsolicited(false);

    std::vector<uint8_t> out(ntf.ByteSize());
    if (ntf.SerializeToArray(out.data(), out.size())) {
        if (send_cb_) send_cb_(GetChannel(), msg::VIDEO_FOCUS_NOTIFICATION, out);
        AA_LOG_I() << "[VideoService] VideoFocusNotification(PROJECTED) sent";
    }
}

void VideoService::SendVideoFocusLoss() {
    aap_protobuf::service::media::video::message::VideoFocusNotification ntf;
    ntf.set_focus(aap_protobuf::service::media::video::message::VIDEO_FOCUS_NATIVE);
    ntf.set_unsolicited(false);

    std::vector<uint8_t> out(ntf.ByteSize());
    if (ntf.SerializeToArray(out.data(), out.size())) {
        if (send_cb_) send_cb_(GetChannel(), msg::VIDEO_FOCUS_NOTIFICATION, out);
        AA_LOG_I() << "[VideoService] VideoFocusNotification(NATIVE) sent";
    }
}

void VideoService::SendMediaAck() {
    aap_protobuf::service::media::source::message::Ack ack;
    ack.set_session_id(session_id_);
    ack.set_ack(1);

    std::vector<uint8_t> out(ack.ByteSize());
    if (ack.SerializeToArray(out.data(), out.size())) {
        if (send_cb_) send_cb_(GetChannel(), msg::MEDIA_ACK, out);
    }
}

void VideoService::FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) {
    auto* sink = service_proto->mutable_media_sink_service();
    sink->set_available_type(aap_protobuf::service::media::shared::message::MEDIA_CODEC_VIDEO_H264_BP);

    auto* video_config = sink->add_video_configs();
    video_config->set_codec_resolution(config_.resolution);
    video_config->set_frame_rate(config_.frame_rate);
    video_config->set_width_margin(config_.width_margin);
    video_config->set_height_margin(config_.height_margin);
    video_config->set_density(config_.density);
}

void VideoService::OnChannelOpened(uint8_t /*channel*/) {}

void VideoService::OnSessionStopped() {
    // Drop the sink reference so no further frames are forwarded after the
    // session is gone. The sink object itself is owned by the app layer
    // and will be released when the app detaches.
    std::shared_ptr<IVideoSink> dead;
    {
        std::lock_guard<std::mutex> lock(sink_mutex_);
        dead = std::move(sink_);
    }
    dead.reset();
}

} // namespace service
} // namespace aauto
