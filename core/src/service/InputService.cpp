#define LOG_TAG "AA.InputService"
#include "aauto/service/InputService.hpp"
#include "aauto/session/AapProtocol.hpp"
#include "aauto/utils/Logger.hpp"
#include "aap_protobuf/service/inputsource/InputSourceService.pb.h"
#include "aap_protobuf/service/inputsource/message/TouchScreenType.pb.h"
#include "aap_protobuf/service/inputsource/message/InputReport.pb.h"
#include "aap_protobuf/service/inputsource/message/TouchEvent.pb.h"
#include "aap_protobuf/service/inputsource/message/PointerAction.pb.h"
#include "aap_protobuf/service/media/sink/message/KeyBindingRequest.pb.h"
#include "aap_protobuf/service/media/sink/message/KeyBindingResponse.pb.h"
#include "aap_protobuf/shared/MessageStatus.pb.h"

#include <chrono>

namespace aauto {
namespace service {

InputService::InputService(core::HeadunitConfig config,
                           std::shared_ptr<platform::IVideoOutput> video_output)
    : config_(std::move(config)), video_output_(std::move(video_output)) {
    // Wire touch events from the UI surface back to the phone
    if (video_output_) {
        video_output_->SetTouchCallback([this](const platform::TouchEvent& e) {
            SendTouchEvent(e.x, e.y, e.pointer_id, e.action);
        });
    }

    namespace msg = session::aap::msg;
    RegisterHandler(msg::INPUT_BINDING_REQUEST, [this](const auto& p){ HandleBindingRequest(p); });
    RegisterHandler(msg::INPUT_EVENT,           [](const auto& p) {
        AA_LOG_D() << "[InputService] InputEvent 수신 (" << p.size() << " bytes)";
    });
}

void InputService::HandleBindingRequest(const std::vector<uint8_t>& payload) {
    aap_protobuf::service::media::sink::message::KeyBindingRequest req;
    if (req.ParseFromArray(payload.data(), payload.size())) {
        AA_LOG_I() << "[InputService] KeyBindingRequest 수신 - keycodes: " << req.keycodes_size();
    }

    aap_protobuf::service::media::sink::message::KeyBindingResponse resp;
    resp.set_status(aap_protobuf::shared::STATUS_SUCCESS);

    std::vector<uint8_t> out(resp.ByteSize());
    if (resp.SerializeToArray(out.data(), out.size())) {
        if (send_cb_) send_cb_(GetChannel(), session::aap::msg::INPUT_BINDING_RESPONSE, out);
        AA_LOG_I() << "[InputService] KeyBindingResponse(OK) 송신 완료";
    }
}

void InputService::SendTouchEvent(int x, int y, int pointer_id, int action) {
    aap_protobuf::service::inputsource::message::InputReport report;

    uint64_t ts = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    report.set_timestamp(ts);

    auto* touch = report.mutable_touch_event();
    touch->set_action(static_cast<aap_protobuf::service::inputsource::message::PointerAction>(action));
    touch->set_action_index(static_cast<uint32_t>(pointer_id));

    auto* ptr = touch->add_pointer_data();
    ptr->set_x(static_cast<uint32_t>(x));
    ptr->set_y(static_cast<uint32_t>(y));
    ptr->set_pointer_id(static_cast<uint32_t>(pointer_id));

    std::vector<uint8_t> out(report.ByteSize());
    if (report.SerializeToArray(out.data(), out.size())) {
        if (send_cb_) send_cb_(GetChannel(), session::aap::msg::INPUT_EVENT, out);
    }
}

void InputService::FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) {
    auto* input = service_proto->mutable_input_source_service();

    for (int code : {3, 4, 19, 20, 21, 22, 23, 66, 84, 85, 87, 88, 5, 6}) {
        input->add_keycodes_supported(code);
    }

    auto* touch = input->add_touchscreen();
    touch->set_width(config_.display_width);
    touch->set_height(config_.display_height);
    touch->set_type(aap_protobuf::service::inputsource::message::CAPACITIVE);
    touch->set_is_secondary(false);
}

} // namespace service
} // namespace aauto
