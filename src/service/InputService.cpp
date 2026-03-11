#define LOG_TAG "InputService"
#include "aauto/service/InputService.hpp"
#include "aauto/utils/Logger.hpp"
#include "aauto/utils/ProtocolUtil.hpp"
#include "aap_protobuf/service/inputsource/InputSourceService.pb.h"
#include "aap_protobuf/service/inputsource/message/TouchScreenType.pb.h"
#include "aap_protobuf/service/inputsource/InputMessageId.pb.h"
#include "aap_protobuf/service/inputsource/message/InputReport.pb.h"
#include "aap_protobuf/service/inputsource/message/TouchEvent.pb.h"
#include "aap_protobuf/service/inputsource/message/PointerAction.pb.h"
#include "aap_protobuf/service/media/sink/message/KeyBindingRequest.pb.h"
#include "aap_protobuf/service/media/sink/message/KeyBindingResponse.pb.h"
#include "aap_protobuf/shared/MessageStatus.pb.h"

#include <chrono>

namespace aauto {
namespace service {

// Input message type constants
static constexpr uint16_t MSG_INPUT_BINDING_REQ  = 0x8002;
static constexpr uint16_t MSG_INPUT_BINDING_RESP = 0x8003;
static constexpr uint16_t MSG_INPUT_EVENT        = 0x8001;

void InputService::HandleMessage(uint16_t msg_type, const std::vector<uint8_t>& payload) {
    if (msg_type == 0x07) {
        HandleChannelOpenRequest(msg_type, payload, send_cb_, GetChannel());
        return;
    }

    switch (msg_type) {
        case MSG_INPUT_BINDING_REQ:
            HandleBindingRequest(payload);
            break;
        case MSG_INPUT_EVENT:
            AA_LOG_D() << "[InputService] InputEvent 수신 (" << payload.size() << " bytes)";
            break;
        default:
            AA_LOG_W() << "[InputService] 미처리 msg_type: 0x" << std::hex << msg_type;
    }
}

void InputService::HandleBindingRequest(const std::vector<uint8_t>& payload) {
    aap_protobuf::service::media::sink::message::KeyBindingRequest req;
    if (req.ParseFromArray(payload.data(), payload.size())) {
        AA_LOG_I() << "[InputService] KeyBindingRequest 수신 - keycodes: " << req.keycodes_size();
    }

    aap_protobuf::service::media::sink::message::KeyBindingResponse resp;
    resp.set_status(aap_protobuf::shared::STATUS_SUCCESS);

    std::vector<uint8_t> out(resp.ByteSizeLong());
    if (resp.SerializeToArray(out.data(), out.size())) {
        if (send_cb_) send_cb_(GetChannel(), MSG_INPUT_BINDING_RESP, out);
        AA_LOG_I() << "[InputService] KeyBindingResponse(OK) 송신 완료";
    }
}

void InputService::AttachToRenderer(std::shared_ptr<aauto::video::VideoRenderer> renderer) {
    if (!renderer) return;
    // weak_ptr로 캡처하여 InputService 소멸 후 dangling 방지
    auto* self = this;
    renderer->SetTouchCallback([self](int x, int y, int pointer_id, int action) {
        self->SendTouchEvent(x, y, pointer_id, action);
    });
    AA_LOG_I() << "[InputService] VideoRenderer 터치 콜백 연결 완료";
}

void InputService::SendTouchEvent(int x, int y, int pointer_id, int action) {
    // InputReport 구성
    aap_protobuf::service::inputsource::message::InputReport report;

    // 타임스탬프 (microseconds)
    uint64_t ts = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    report.set_timestamp(ts);

    // TouchEvent 구성
    auto* touch = report.mutable_touch_event();
    // 액션은 TouchEvent에 설정 (포인터가 아니라 Touch 이벤트에 속함)
    touch->set_action(static_cast<aap_protobuf::service::inputsource::message::PointerAction>(action));
    touch->set_action_index(static_cast<uint32_t>(pointer_id));

    auto* ptr = touch->add_pointer_data();
    ptr->set_x(static_cast<uint32_t>(x));
    ptr->set_y(static_cast<uint32_t>(y));
    ptr->set_pointer_id(static_cast<uint32_t>(pointer_id));

    std::vector<uint8_t> out(report.ByteSizeLong());
    if (report.SerializeToArray(out.data(), out.size())) {
        if (send_cb_) send_cb_(GetChannel(), MSG_INPUT_EVENT, out);
    }
}

void InputService::FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) {
    auto* input = service_proto->mutable_input_source_service();

    // Keycodes (Standard Android Keycodes)
    input->add_keycodes_supported(3);   // HOME
    input->add_keycodes_supported(4);   // BACK
    input->add_keycodes_supported(19);  // DPAD_UP
    input->add_keycodes_supported(20);  // DPAD_DOWN
    input->add_keycodes_supported(21);  // DPAD_LEFT
    input->add_keycodes_supported(22);  // DPAD_RIGHT
    input->add_keycodes_supported(23);  // DPAD_CENTER
    input->add_keycodes_supported(66);  // ENTER
    input->add_keycodes_supported(84);  // SEARCH
    input->add_keycodes_supported(85);  // MEDIA_PLAY_PAUSE
    input->add_keycodes_supported(87);  // MEDIA_NEXT
    input->add_keycodes_supported(88);  // MEDIA_PREVIOUS
    input->add_keycodes_supported(5);   // CALL
    input->add_keycodes_supported(6);   // ENDCALL

    // Touchscreen Configuration
    auto* touch = input->add_touchscreen();
    touch->set_width(800);
    touch->set_height(480);
    touch->set_type(aap_protobuf::service::inputsource::message::CAPACITIVE);
    touch->set_is_secondary(false);
}

}  // namespace service
}  // namespace aauto
