#define LOG_TAG "ControlService"
#include "aauto/service/ControlService.hpp"
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

void ControlService::HandleMessage(uint16_t msg_type, const std::vector<uint8_t>& payload) {
    // Session에서 이미 한 번 로깅하므로 여기서는 상세 정보만 로깅
    
    if (msg_type == session::aap::TYPE_SERVICE_DISCOVERY_REQ) { // 0x05
        aap_protobuf::service::control::message::ServiceDiscoveryRequest sd_req;
        if (sd_req.ParseFromArray(payload.data(), payload.size())) {
            std::cout << "  폰 정보: " << sd_req.device_name() << " (" << sd_req.label_text() << ")\n";
            SendServiceDiscoveryResponse();
        } else {
            std::cerr << "  ServiceDiscoveryRequest 파싱 실패!\n";
        }
    } else if (msg_type == 0x0D) { // NAVFOCUSREQUESTNOTIFICATION
        std::cout << "  NAVFOCUSREQUESTNOTIFICATION 수신 (0x0D)\n";
        SendNavFocusNotification(1); // NAV_FOCUS_1
    } else if (msg_type == 0x12) { // AUDIOFOCUSREQUESTNOTFICATION
        std::cout << "  AUDIOFOCUSREQUESTNOTFICATION 수신 (0x12)\n";
        aap_protobuf::service::control::message::AudioFocusRequestNotification af_req;
        if (af_req.ParseFromArray(payload.data(), payload.size())) {
            std::cout << "  요청받은 포커스 타입: " << af_req.request() << "\n";
            
            aap_protobuf::service::control::message::AudioFocusNotification af_resp;
            int state = 1; // STATE_GAIN
            if (af_req.request() == 4) state = 3; // RELEASE -> STATE_LOSS
            
            af_resp.set_focus_state(static_cast<aap_protobuf::service::control::message::AudioFocusStateType>(state));
            af_resp.set_unsolicited(false);

            std::vector<uint8_t> out_payload(af_resp.ByteSizeLong());
            if (af_resp.SerializeToArray(out_payload.data(), out_payload.size())) {
                if (send_cb_) send_cb_(session::aap::CH_CONTROL, 0x13, out_payload);
                AA_LOG_I() << "  AUDIOFOCUSNOTFICATION(" << state << ") 응답 송신 완료";
            }
        }
    } else if (msg_type == 0x0B) { // PINGREQUEST
        aap_protobuf::service::control::message::PingRequest ping_req;
        if (ping_req.ParseFromArray(payload.data(), payload.size())) {
            aap_protobuf::service::control::message::PingResponse ping_resp;
            ping_resp.set_timestamp(ping_req.timestamp());

            std::vector<uint8_t> out_payload(ping_resp.ByteSizeLong());
            if (ping_resp.SerializeToArray(out_payload.data(), out_payload.size())) {
                if (send_cb_) send_cb_(session::aap::CH_CONTROL, 0x0C, out_payload);
                std::cout << "  PINGRESPONSE 회신 완료 (Timestamp: " << ping_req.timestamp() << ")\n";
            }
        }
    }
}

void ControlService::SendServiceDiscoveryResponse() {
    if (!send_cb_) return;
    AA_LOG_I() << "[ControlService] Sending ServiceDiscoveryResponse...";

    aap_protobuf::service::control::message::ServiceDiscoveryResponse sd_resp;
    auto* hu_info = sd_resp.mutable_headunit_info();
    hu_info->set_make("Google");
    hu_info->set_model("PixelSim");
    hu_info->set_year("2026");
    hu_info->set_vehicle_id("VIN1234567890AA");
    hu_info->set_head_unit_make("OpenSource");
    hu_info->set_head_unit_model("AAuto");
    hu_info->set_head_unit_software_build("1.0.0");
    hu_info->set_head_unit_software_version("1.0");

    sd_resp.set_driver_position(aap_protobuf::service::control::message::DRIVER_POSITION_LEFT);
    sd_resp.set_session_configuration(0); // false for both clock and native media

    if (service_provider_) {
        auto services = service_provider_();
        for (const auto& svc : services) {
            if (svc->GetType() == ServiceType::CONTROL) continue;

            auto* service_proto = sd_resp.add_channels();
            service_proto->set_id(svc->GetChannel());
            svc->FillServiceDefinition(service_proto);
        }
    }

    std::vector<uint8_t> out_payload(sd_resp.ByteSizeLong());
    if (sd_resp.SerializeToArray(out_payload.data(), out_payload.size())) {
        AA_LOG_I() << "[ControlService] ServiceDiscoveryResponse 송신 완료";
        send_cb_(session::aap::CH_CONTROL, session::aap::TYPE_SERVICE_DISCOVERY_RESP, out_payload);
    }
}

void ControlService::SendNavFocusNotification(int type) {
    // NAVFOCUSRNOTIFICATION = 0x0E
    aap_protobuf::service::control::message::NavFocusNotification ntf;
    ntf.set_focus_type(static_cast<aap_protobuf::service::control::message::NavFocusType>(type));

    std::vector<uint8_t> out_payload(ntf.ByteSizeLong());
    if (ntf.SerializeToArray(out_payload.data(), out_payload.size())) {
        if (send_cb_) send_cb_(session::aap::CH_CONTROL, 0x0E, out_payload);
        AA_LOG_I() << "  [ControlService] NavFocusNotification(" << type << ") 송신 완료";
    }
}

void ControlService::SendAudioFocusNotification(int state) {
    aap_protobuf::service::control::message::AudioFocusNotification af_resp;
    af_resp.set_focus_state(static_cast<aap_protobuf::service::control::message::AudioFocusStateType>(state));
    //af_resp.set_unsolicited(true);

    std::vector<uint8_t> out_payload(af_resp.ByteSizeLong());
    if (af_resp.SerializeToArray(out_payload.data(), out_payload.size())) {
        if (send_cb_) send_cb_(session::aap::CH_CONTROL, 0x13, out_payload); // AUDIOFOCUSNOTFICATION (0x13)
        AA_LOG_I() << "  [ControlService] Unsolicited AudioFocusNotification(" << state << ") 송신 완료";
    }
}

}  // namespace service
}  // namespace aauto
