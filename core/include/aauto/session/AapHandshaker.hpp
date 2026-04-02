#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "aauto/crypto/CryptoManager.hpp"
#include "aauto/transport/ITransport.hpp"

namespace aauto {
namespace session {

// Executes the AAP connection handshake over an already-connected transport:
//   1. Version exchange
//   2. SSL/TLS handshake
//   3. Auth-complete notification
//
// Any bytes received during the handshake that belong to the normal message
// stream are returned via LeftoverBytes() so the caller can seed them into
// the MessageFramer.
class AapHandshaker {
   public:
    AapHandshaker(transport::ITransport& transport, crypto::CryptoManager& crypto);

    // Runs all three handshake stages synchronously.
    // Returns true on success; on failure the session should be torn down.
    bool Run();

    // Bytes received during handshake that were not consumed by the protocol.
    // Move these into the MessageFramer after a successful Run().
    std::vector<uint8_t> TakeLeftoverBytes() { return std::move(leftover_); }

   private:
    bool DoVersionExchange();
    bool DoSslHandshake();
    bool SendAuthComplete();

    // Read from transport into buf; return false on empty/error.
    bool ReadInto(std::vector<uint8_t>& buf);

    // Walk complete AAP packets in buf, calling visitor(channel, msg_type, payload) for each.
    // visitor returns true to continue, false to stop.
    // Consumed packets are erased from buf; an incomplete trailing packet is left intact.
    using PacketVisitor = std::function<bool(uint8_t, uint16_t, const std::vector<uint8_t>&)>;
    void WalkPackets(std::vector<uint8_t>& buf, const PacketVisitor& visitor);

    // Walk buf; route the first packet matching target_msg_type to out_payload (optional)
    // and stop. All other packets are forwarded to leftover_.
    // Returns true if the target was found.
    bool DrainUntil(std::vector<uint8_t>& buf, uint16_t target_msg_type,
                    std::vector<uint8_t>* out_payload = nullptr);

    transport::ITransport&  transport_;
    crypto::CryptoManager&  crypto_;
    std::vector<uint8_t>    leftover_;
};

} // namespace session
} // namespace aauto
