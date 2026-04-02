#define LOG_TAG "AA.ServiceBase"
#include "aauto/service/ServiceBase.hpp"
#include "aauto/session/AapProtocol.hpp"
#include "aauto/utils/Logger.hpp"
#include "aauto/utils/ProtocolUtil.hpp"
#include "aap_protobuf/service/control/message/ChannelOpenRequest.pb.h"
#include "aap_protobuf/service/control/message/ChannelOpenResponse.pb.h"

namespace aauto {
namespace service {

void ServiceBase::RegisterHandler(uint16_t msg_type, MessageHandler handler) {
    handlers_[msg_type] = std::move(handler);
}

void ServiceBase::HandleMessage(uint16_t msg_type, const std::vector<uint8_t>& payload) {
    if (msg_type == session::aap::msg::CHANNEL_OPEN_REQUEST) {
        DispatchChannelOpen(payload);
        return;
    }

    auto it = handlers_.find(msg_type);
    if (it != handlers_.end()) {
        it->second(payload);
    } else {
        AA_LOG_W() << "[" << GetName() << "] unhandled msg_type: 0x" << std::hex << msg_type;
    }
}

void ServiceBase::DispatchChannelOpen(const std::vector<uint8_t>& payload) {
    aap_protobuf::service::control::message::ChannelOpenRequest req;
    if (!req.ParseFromArray(payload.data(), payload.size())) return;

    OnChannelOpened(channel_);

    aap_protobuf::service::control::message::ChannelOpenResponse resp;
    resp.set_status(aap_protobuf::shared::STATUS_SUCCESS);

    std::vector<uint8_t> out(resp.ByteSize());
    if (resp.SerializeToArray(out.data(), out.size())) {
        if (send_cb_) send_cb_(channel_, session::aap::msg::CHANNEL_OPEN_RESPONSE, out);
        AA_LOG_I() << "[" << GetName() << "] ChannelOpenResponse sent (Ch:"
                   << utils::ProtocolUtil::GetChannelName(channel_) << ")";
    }
}

} // namespace service
} // namespace aauto
