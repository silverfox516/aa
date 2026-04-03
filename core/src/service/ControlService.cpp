#define LOG_TAG "AA.ControlService"
#include "aauto/service/ControlService.hpp"
#include "aauto/session/AapProtocol.hpp"
#include "aauto/utils/Logger.hpp"
#include "aauto/utils/ProtocolUtil.hpp"
#include "aap_protobuf/service/control/message/ServiceDiscoveryRequest.pb.h"
#include "aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h"
#include "aap_protobuf/service/control/message/AudioFocusRequestNotification.pb.h"
#include "aap_protobuf/service/control/message/AudioFocusNotification.pb.h"
#include "aap_protobuf/service/control/message/PingRequest.pb.h"
#include "aap_protobuf/service/control/message/PingResponse.pb.h"
#include "aap_protobuf/service/control/message/NavFocusNotification.pb.h"
#include "aap_protobuf/service/control/message/NavFocusType.pb.h"
#include "aap_protobuf/service/control/message/AudioFocusStateType.pb.h"

#include <chrono>

namespace aauto {
namespace service {

namespace msg = session::aap::msg;

ControlService::ControlService(core::HeadunitConfig config,
                               std::vector<std::shared_ptr<IService>> peer_services)
    : config_(std::move(config))
    , peer_services_(std::move(peer_services)) {

    RegisterHandler(msg::SERVICE_DISCOVERY_REQ, [this](const auto& p) {
        aap_protobuf::service::control::message::ServiceDiscoveryRequest sd_req;
        if (sd_req.ParseFromArray(p.data(), p.size())) {
            AA_LOG_I() << "  Phone: " << sd_req.device_name() << " (" << sd_req.label_text() << ")";
            SendServiceDiscoveryResponse();
        }
    });
    RegisterHandler(msg::NAV_FOCUS_REQUEST, [this](const auto&) {
        SendNavFocusNotification(1);
    });
    RegisterHandler(msg::AUDIO_FOCUS_REQUEST, [this](const auto& p) {
        namespace af = aap_protobuf::service::control::message;
        af::AudioFocusRequestNotification af_req;
        if (!af_req.ParseFromArray(p.data(), p.size())) return;

        // Respond based on whether the HU has granted audio focus (surface is active).
        // RELEASE is always honoured. All other requests succeed only when focus is granted.
        af::AudioFocusStateType state;
        if (af_req.request() == af::AUDIO_FOCUS_RELEASE) {
            state = af::AUDIO_FOCUS_STATE_LOSS;
        } else if (audio_focus_granted_) {
            switch (af_req.request()) {
                case af::AUDIO_FOCUS_GAIN_TRANSIENT:
                    state = af::AUDIO_FOCUS_STATE_GAIN_TRANSIENT;
                    break;
                case af::AUDIO_FOCUS_GAIN_TRANSIENT_MAY_DUCK:
                    state = af::AUDIO_FOCUS_STATE_GAIN_TRANSIENT_GUIDANCE_ONLY;
                    break;
                default:
                    state = af::AUDIO_FOCUS_STATE_GAIN;
                    break;
            }
        } else {
            // No surface active — deny focus so the phone does not stream audio.
            state = af::AUDIO_FOCUS_STATE_LOSS;
            AA_LOG_I() << "[ControlService] AudioFocusRequest denied (no surface)";
        }

        af::AudioFocusNotification af_resp;
        af_resp.set_focus_state(state);
        af_resp.set_unsolicited(false);
        std::vector<uint8_t> out(af_resp.ByteSize());
        if (af_resp.SerializeToArray(out.data(), out.size())) {
            if (send_cb_) send_cb_(session::aap::CH_CONTROL, msg::AUDIO_FOCUS_NOTIFICATION, out);
        }
    });
    RegisterHandler(msg::PING_REQUEST, [this](const auto& p) {
        aap_protobuf::service::control::message::PingRequest ping_req;
        if (ping_req.ParseFromArray(p.data(), p.size())) {
            aap_protobuf::service::control::message::PingResponse ping_resp;
            ping_resp.set_timestamp(ping_req.timestamp());
            std::vector<uint8_t> out(ping_resp.ByteSize());
            if (ping_resp.SerializeToArray(out.data(), out.size())) {
                if (send_cb_) send_cb_(session::aap::CH_CONTROL, msg::PING_RESPONSE, out);
            }
        }
    });
    RegisterHandler(msg::PING_RESPONSE, [](const auto&) {});
}

void ControlService::SendServiceDiscoveryResponse() {
    if (!send_cb_) return;
    AA_LOG_I() << "[ControlService] Sending ServiceDiscoveryResponse...";

    aap_protobuf::service::control::message::ServiceDiscoveryResponse sd_resp;
    auto* hu_info = sd_resp.mutable_headunit_info();
    hu_info->set_make(config_.vehicle_make);
    hu_info->set_model(config_.vehicle_model);
    hu_info->set_year(config_.vehicle_year);
    hu_info->set_vehicle_id(config_.vehicle_id);
    hu_info->set_head_unit_make(config_.head_unit_make);
    hu_info->set_head_unit_model(config_.head_unit_model);
    hu_info->set_head_unit_software_build(config_.head_unit_software_build);
    hu_info->set_head_unit_software_version(config_.head_unit_software_version);

    sd_resp.set_driver_position(aap_protobuf::service::control::message::DRIVER_POSITION_LEFT);
    sd_resp.set_session_configuration(0);

    for (const auto& svc : peer_services_) {
        auto* svc_proto = sd_resp.add_channels();
        svc_proto->set_id(svc->GetChannel());
        svc->FillServiceDefinition(svc_proto);
    }

    std::vector<uint8_t> out(sd_resp.ByteSize());
    if (sd_resp.SerializeToArray(out.data(), out.size())) {
        send_cb_(session::aap::CH_CONTROL, msg::SERVICE_DISCOVERY_RESP, out);
        AA_LOG_I() << "[ControlService] ServiceDiscoveryResponse sent";
    }
}

ControlService::~ControlService() {
    OnSessionStopped();
}

void ControlService::OnChannelOpened(uint8_t /*channel*/) {
    heartbeat_running_.store(true);
    heartbeat_thread_ = std::thread(&ControlService::HeartbeatLoop, this);
}

void ControlService::OnSessionStopped() {
    heartbeat_running_.store(false);
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
}

void ControlService::SendPing() {
    if (!send_cb_) return;

    aap_protobuf::service::control::message::PingRequest ping;
    ping.set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());

    std::vector<uint8_t> payload(ping.ByteSize());
    if (ping.SerializeToArray(payload.data(), payload.size()))
        send_cb_(session::aap::CH_CONTROL, session::aap::msg::PING_REQUEST, payload);
}

void ControlService::HeartbeatLoop() {
    while (heartbeat_running_.load()) {
        for (int i = 0; i < 50 && heartbeat_running_.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (heartbeat_running_.load()) SendPing();
    }
}

void ControlService::SendAudioFocusGain() {
    namespace af = aap_protobuf::service::control::message;
    audio_focus_granted_ = true;
    SendAudioFocusNotification(static_cast<int>(af::AUDIO_FOCUS_STATE_GAIN));
}

void ControlService::SendAudioFocusLoss() {
    namespace af = aap_protobuf::service::control::message;
    audio_focus_granted_ = false;
    SendAudioFocusNotification(static_cast<int>(af::AUDIO_FOCUS_STATE_LOSS));
}

void ControlService::SendNavFocusNotification(int type) {
    aap_protobuf::service::control::message::NavFocusNotification ntf;
    ntf.set_focus_type(static_cast<aap_protobuf::service::control::message::NavFocusType>(type));

    std::vector<uint8_t> out(ntf.ByteSize());
    if (ntf.SerializeToArray(out.data(), out.size())) {
        if (send_cb_) send_cb_(session::aap::CH_CONTROL, msg::NAV_FOCUS_NOTIFICATION, out);
        AA_LOG_I() << "[ControlService] NavFocusNotification(" << type << ") sent";
    }
}

void ControlService::SendAudioFocusNotification(int state) {
    aap_protobuf::service::control::message::AudioFocusNotification af_resp;
    af_resp.set_focus_state(
        static_cast<aap_protobuf::service::control::message::AudioFocusStateType>(state));

    std::vector<uint8_t> out(af_resp.ByteSize());
    if (af_resp.SerializeToArray(out.data(), out.size())) {
        if (send_cb_) send_cb_(session::aap::CH_CONTROL, msg::AUDIO_FOCUS_NOTIFICATION, out);
        AA_LOG_I() << "[ControlService] AudioFocusNotification(" << state << ") sent";
    }
}

} // namespace service
} // namespace aauto
