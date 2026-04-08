#define LOG_TAG "AA.CORE.ControlService"
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

            if (phone_info_cb_) {
                session::PhoneInfo info;
                info.device_name = sd_req.device_name();
                info.label_text  = sd_req.label_text();
                if (sd_req.has_phone_info()) {
                    const auto& pi = sd_req.phone_info();
                    if (pi.has_instance_id())              info.instance_id              = pi.instance_id();
                    if (pi.has_connectivity_lifetime_id()) info.connectivity_lifetime_id = pi.connectivity_lifetime_id();
                }
                phone_info_cb_(info);
            }

            SendServiceDiscoveryResponse();
        }
    });
    RegisterHandler(msg::NAV_FOCUS_REQUEST, [this](const auto&) {
        // Always grant projected nav focus — Android Auto's navigation
        // (the phone's mapping app) drives turn-by-turn on the HU display.
        // NAV_FOCUS_NATIVE would tell the phone the HU has its own native
        // navigation in foreground, suppressing AA guidance.
        SendNavFocusNotification(
            aap_protobuf::service::control::message::NAV_FOCUS_PROJECTED);
    });
    RegisterHandler(msg::AUDIO_FOCUS_REQUEST, [this](const auto& p) {
        namespace af = aap_protobuf::service::control::message;
        af::AudioFocusRequestNotification af_req;
        if (!af_req.ParseFromArray(p.data(), p.size())) return;

        // Audio focus is granted unconditionally and the response simply
        // mirrors the request type. The AAP audio focus message has no
        // stream/channel id (it is session-wide) and phones treat a LOSS
        // response as a transient denial, immediately retrying in a tight
        // loop. With the sink model, audio data is dropped harmlessly when
        // no sink is attached, so default-grant is both simpler and avoids
        // the loop. Explicit revocation (phone-call interruption etc) will
        // be added as a separate API in a later phase.
        af::AudioFocusStateType state;
        switch (af_req.request()) {
            case af::AUDIO_FOCUS_RELEASE:
                state = af::AUDIO_FOCUS_STATE_LOSS;
                break;
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
        AA_LOG_I() << "[ControlService] AudioFocusRequest -> grant (type=" << af_req.request() << ")";

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

void ControlService::SendNavFocusNotification(
        aap_protobuf::service::control::message::NavFocusType type) {
    aap_protobuf::service::control::message::NavFocusNotification ntf;
    ntf.set_focus_type(type);

    std::vector<uint8_t> out(ntf.ByteSize());
    if (ntf.SerializeToArray(out.data(), out.size())) {
        if (send_cb_) send_cb_(session::aap::CH_CONTROL, msg::NAV_FOCUS_NOTIFICATION, out);
        AA_LOG_I() << "[ControlService] NavFocusNotification(type=" << static_cast<int>(type) << ") sent";
    }
}

} // namespace service
} // namespace aauto
