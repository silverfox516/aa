#define LOG_TAG "AA.CORE.ControlService"
#include "aauto/service/ControlService.hpp"
#include "aauto/session/AapProtocol.hpp"
#include "aauto/utils/Logger.hpp"
#include "aauto/utils/ProtocolUtil.hpp"
#include "aap_protobuf/service/control/message/ServiceDiscoveryRequest.pb.h"
#include "aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h"
#include "aap_protobuf/service/control/message/AudioFocusRequest.pb.h"
#include "aap_protobuf/service/control/message/AudioFocusNotification.pb.h"
#include "aap_protobuf/service/control/message/ByeByeRequest.pb.h"
#include "aap_protobuf/service/control/message/ByeByeResponse.pb.h"
#include "aap_protobuf/service/control/message/PingRequest.pb.h"
#include "aap_protobuf/service/control/message/PingResponse.pb.h"
#include "aap_protobuf/service/control/message/NavFocusNotification.pb.h"
#include "aap_protobuf/service/control/message/NavFocusRequestNotification.pb.h"
#include "aap_protobuf/service/control/message/NavFocusType.pb.h"
#include "aap_protobuf/service/control/message/AudioFocusStateType.pb.h"
#include "aap_protobuf/service/control/message/VoiceSessionNotification.pb.h"
#include "aap_protobuf/service/control/message/BatteryStatusNotification.pb.h"

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

            session::PhoneInfo info;
            info.device_name = sd_req.device_name();
            info.label_text  = sd_req.label_text();
            if (sd_req.has_phone_info()) {
                const auto& pi = sd_req.phone_info();
                if (pi.has_instance_id())              info.instance_id              = pi.instance_id();
                if (pi.has_connectivity_lifetime_id()) info.connectivity_lifetime_id = pi.connectivity_lifetime_id();
            }

            // Promote the session log tag to include the phone name BEFORE
            // logging the mapping line, so that mapping line itself shows
            // the new tag.
            if (phone_tag_cb_) {
                std::string label = !info.device_name.empty() ? info.device_name
                                  : !info.label_text.empty()  ? info.label_text
                                  : "phone";
                phone_tag_cb_(label);
            }
            AA_LOG_I() << "Session phone identified: device=\"" << info.device_name
                       << "\" label=\"" << info.label_text
                       << "\" instance=" << info.instance_id;

            if (phone_info_cb_) phone_info_cb_(info);

            SendServiceDiscoveryResponse();
        }
    });
    RegisterHandler(msg::NAV_FOCUS_REQUEST, [this](const auto& p) {
        namespace ctrl = aap_protobuf::service::control::message;
        ctrl::NavFocusRequestNotification req;
        if (req.ParseFromArray(p.data(), p.size())) {
            AA_LOG_I() << "[ControlService] NavFocusRequest type=" << req.focus_type();
        }
        // Always grant projected nav focus — Android Auto's navigation
        // (the phone's mapping app) drives turn-by-turn on the HU display.
        SendNavFocusNotification(ctrl::NAV_FOCUS_PROJECTED);
    });
    RegisterHandler(msg::AUDIO_FOCUS_REQUEST, [this](const auto& p) {
        namespace af = aap_protobuf::service::control::message;
        af::AudioFocusRequest af_req;
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
        switch (af_req.audio_focus_type()) {
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
        AA_LOG_I() << "[ControlService] AudioFocusRequest -> grant (type=" << af_req.audio_focus_type() << ")";

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
    RegisterHandler(msg::PING_RESPONSE, [this](const auto&) {
        // Liveness signal — feed the heartbeat watchdog.
        last_pong_ns_.store(std::chrono::steady_clock::now().time_since_epoch().count());
    });
    RegisterHandler(msg::BYEBYE_REQUEST, [this](const auto& p) {
        namespace ctrl = aap_protobuf::service::control::message;
        ctrl::ByeByeRequest req;
        if (req.ParseFromArray(p.data(), p.size())) {
            AA_LOG_I() << "[ControlService] ByeByeRequest received, reason=" << req.reason();
        } else {
            AA_LOG_W() << "[ControlService] ByeByeRequest parse failed, closing anyway";
        }

        // Acknowledge before tearing down.
        ctrl::ByeByeResponse resp;
        std::vector<uint8_t> out(resp.ByteSize());
        if (resp.SerializeToArray(out.data(), out.size())) {
            if (send_cb_) send_cb_(session::aap::CH_CONTROL, msg::BYEBYE_RESPONSE, out);
        }

        TriggerSessionClose("ByeByeRequest");
    });
    RegisterHandler(msg::BYEBYE_RESPONSE, [this](const auto&) {
        TriggerSessionClose("ByeByeResponse");
    });
    // Phone notifications we log but don't act on.
    RegisterHandler(msg::VOICE_SESSION_NOTIFICATION, [](const auto& p) {
        namespace ctrl = aap_protobuf::service::control::message;
        ctrl::VoiceSessionNotification ntf;
        if (ntf.ParseFromArray(p.data(), p.size())) {
            AA_LOG_I() << "[ControlService] VoiceSessionNotification status=" << ntf.status();
        } else {
            AA_LOG_W() << "[ControlService] VoiceSessionNotification parse failed (" << p.size() << "B)";
        }
    });
    RegisterHandler(msg::BATTERY_STATUS_NOTIFICATION, [](const auto& p) {
        namespace ctrl = aap_protobuf::service::control::message;
        ctrl::BatteryStatusNotification ntf;
        if (ntf.ParseFromArray(p.data(), p.size())) {
            AA_LOG_I() << "[ControlService] BatteryStatusNotification level=" << ntf.battery_level()
                       << "% time_remaining_s=" << (ntf.has_time_remaining_s() ? static_cast<int64_t>(ntf.time_remaining_s()) : -1)
                       << " critical=" << (ntf.has_critical_battery() ? ntf.critical_battery() : false);
        } else {
            AA_LOG_W() << "[ControlService] BatteryStatusNotification parse failed (" << p.size() << "B)";
        }
    });
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

    auto* conn_cfg = sd_resp.mutable_connection_configuration();
    auto* ping_cfg = conn_cfg->mutable_ping_configuration();
    ping_cfg->set_tracked_ping_count(5);
    ping_cfg->set_timeout_ms(3000);
    ping_cfg->set_interval_ms(1000);
    ping_cfg->set_high_latency_threshold_ms(200);

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
    last_pong_ns_.store(std::chrono::steady_clock::now().time_since_epoch().count());
    close_triggered_.store(false);
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
        std::chrono::steady_clock::now().time_since_epoch()).count());

    std::vector<uint8_t> payload(ping.ByteSize());
    if (ping.SerializeToArray(payload.data(), payload.size()))
        send_cb_(session::aap::CH_CONTROL, session::aap::msg::PING_REQUEST, payload);
}

void ControlService::HeartbeatLoop() {
    while (heartbeat_running_.load()) {
        // Sleep in 100 ms slices so Stop() can wake us promptly.
        const auto slice = std::chrono::milliseconds(100);
        const int slices = static_cast<int>(kPingInterval / slice);
        for (int i = 0; i < slices && heartbeat_running_.load(); ++i)
            std::this_thread::sleep_for(slice);
        if (!heartbeat_running_.load()) break;

        SendPing();

        // Watchdog: if the phone has not pong'd within kPingTimeout, the
        // session is dead even though the transport may still be open.
        auto last_pong = std::chrono::steady_clock::time_point(
            std::chrono::steady_clock::duration(last_pong_ns_.load()));
        auto elapsed = std::chrono::steady_clock::now() - last_pong;
        if (elapsed > kPingTimeout) {
            AA_LOG_E() << "[ControlService] Ping timeout — last pong "
                       << std::chrono::duration_cast<std::chrono::seconds>(elapsed).count()
                       << "s ago. Closing session.";
            TriggerSessionClose("PingTimeout");
            return;
        }
    }
}

void ControlService::TriggerSessionClose(const char* reason) {
    if (close_triggered_.exchange(true)) return;
    AA_LOG_I() << "[ControlService] TriggerSessionClose: " << reason;
    if (session_close_cb_) session_close_cb_();
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
