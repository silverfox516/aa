#define LOG_TAG "ControlService"
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
            AA_LOG_I() << "  폰 정보: " << sd_req.device_name() << " (" << sd_req.label_text() << ")";
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

        af::AudioFocusStateType state;
        switch (af_req.request()) {
            case af::AUDIO_FOCUS_GAIN:
                state = af::AUDIO_FOCUS_STATE_GAIN;
                break;
            case af::AUDIO_FOCUS_GAIN_TRANSIENT:
                state = af::AUDIO_FOCUS_STATE_GAIN_TRANSIENT;
                break;
            case af::AUDIO_FOCUS_GAIN_TRANSIENT_MAY_DUCK:
                state = af::AUDIO_FOCUS_STATE_GAIN_TRANSIENT_GUIDANCE_ONLY;
                break;
            case af::AUDIO_FOCUS_RELEASE:
                state = af::AUDIO_FOCUS_STATE_LOSS;
                break;
            default:
                state = af::AUDIO_FOCUS_STATE_GAIN;
                break;
        }

        af::AudioFocusNotification af_resp;
        af_resp.set_focus_state(state);
        af_resp.set_unsolicited(false);
        std::vector<uint8_t> out(af_resp.ByteSizeLong());
        if (af_resp.SerializeToArray(out.data(), out.size())) {
            if (send_cb_) send_cb_(session::aap::CH_CONTROL, msg::AUDIO_FOCUS_NOTIFICATION, out);
        }
    });
    RegisterHandler(msg::PING_REQUEST, [this](const auto& p) {
        aap_protobuf::service::control::message::PingRequest ping_req;
        if (ping_req.ParseFromArray(p.data(), p.size())) {
            aap_protobuf::service::control::message::PingResponse ping_resp;
            ping_resp.set_timestamp(ping_req.timestamp());
            std::vector<uint8_t> out(ping_resp.ByteSizeLong());
            if (ping_resp.SerializeToArray(out.data(), out.size())) {
                if (send_cb_) send_cb_(session::aap::CH_CONTROL, msg::PING_RESPONSE, out);
            }
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

    for (const auto& svc : peer_services_) {
        auto* svc_proto = sd_resp.add_channels();
        svc_proto->set_id(svc->GetChannel());
        svc->FillServiceDefinition(svc_proto);
    }

    std::vector<uint8_t> out(sd_resp.ByteSizeLong());
    if (sd_resp.SerializeToArray(out.data(), out.size())) {
        send_cb_(session::aap::CH_CONTROL, msg::SERVICE_DISCOVERY_RESP, out);
        AA_LOG_I() << "[ControlService] ServiceDiscoveryResponse 송신 완료";
    }
}

void ControlService::SendNavFocusNotification(int type) {
    aap_protobuf::service::control::message::NavFocusNotification ntf;
    ntf.set_focus_type(static_cast<aap_protobuf::service::control::message::NavFocusType>(type));

    std::vector<uint8_t> out(ntf.ByteSizeLong());
    if (ntf.SerializeToArray(out.data(), out.size())) {
        if (send_cb_) send_cb_(session::aap::CH_CONTROL, msg::NAV_FOCUS_NOTIFICATION, out);
        AA_LOG_I() << "[ControlService] NavFocusNotification(" << type << ") 송신 완료";
    }
}

void ControlService::SendAudioFocusNotification(int state) {
    aap_protobuf::service::control::message::AudioFocusNotification af_resp;
    af_resp.set_focus_state(
        static_cast<aap_protobuf::service::control::message::AudioFocusStateType>(state));

    std::vector<uint8_t> out(af_resp.ByteSizeLong());
    if (af_resp.SerializeToArray(out.data(), out.size())) {
        if (send_cb_) send_cb_(session::aap::CH_CONTROL, msg::AUDIO_FOCUS_NOTIFICATION, out);
        AA_LOG_I() << "[ControlService] AudioFocusNotification(" << state << ") 송신 완료";
    }
}

} // namespace service
} // namespace aauto
