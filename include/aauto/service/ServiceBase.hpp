#pragma once

#include "aauto/service/IService.hpp"

namespace aauto {
namespace service {

// Concrete base for all services.
// Eliminates the send_cb_ / SetSendCallback / PrepareMessage boilerplate
// and provides DispatchChannelOpen for CHANNEL_OPEN_REQUEST handling.
class ServiceBase : public IService {
   public:
    void SetSendCallback(SendCallback cb) override { send_cb_ = std::move(cb); }
    std::vector<uint8_t> PrepareMessage(const std::vector<uint8_t>& payload) override { return payload; }

   protected:
    // Call this at the top of HandleMessage when msg_type == CHANNEL_OPEN_REQUEST.
    // Invokes OnChannelOpened() then sends ChannelOpenResponse.
    void DispatchChannelOpen(const std::vector<uint8_t>& payload);

    SendCallback send_cb_;
};

} // namespace service
} // namespace aauto
