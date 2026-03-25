#pragma once

#include <unordered_map>
#include "aauto/service/IService.hpp"

namespace aauto {
namespace service {

// Concrete base for all services.
// - Eliminates SetSendCallback / PrepareMessage boilerplate.
// - Provides a dispatch table via RegisterHandler(): subclasses register
//   per-message-type handlers in their constructor; HandleMessage() is
//   handled here and must NOT be overridden.
// - CHANNEL_OPEN_REQUEST is handled automatically.
// - Unregistered message types emit a warning log automatically.
class ServiceBase : public IService {
   public:
    void SetSendCallback(SendCallback cb) override { send_cb_ = std::move(cb); }
    std::vector<uint8_t> PrepareMessage(const std::vector<uint8_t>& payload) override { return payload; }

    // Final: subclasses must NOT override. Register handlers in the constructor instead.
    void HandleMessage(uint16_t msg_type, const std::vector<uint8_t>& payload) final;

   protected:
    using MessageHandler = std::function<void(const std::vector<uint8_t>&)>;

    // Register a handler for a specific message type.
    // Call this in the subclass constructor.
    void RegisterHandler(uint16_t msg_type, MessageHandler handler);

    // Invokes OnChannelOpened() then sends ChannelOpenResponse.
    void DispatchChannelOpen(const std::vector<uint8_t>& payload);

    SendCallback send_cb_;

   private:
    std::unordered_map<uint16_t, MessageHandler> handlers_;
};

} // namespace service
} // namespace aauto
