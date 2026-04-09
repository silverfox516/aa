#define LOG_TAG "AA.CORE.AapHandshaker"
#include "aauto/session/AapHandshaker.hpp"
#include "aauto/session/AapProtocol.hpp"
#include "aauto/utils/Logger.hpp"
#include "aap_protobuf/service/control/message/AuthResponse.pb.h"

#include <thread>
#include <chrono>

namespace aauto {
namespace session {

AapHandshaker::AapHandshaker(transport::ITransport& transport, crypto::CryptoManager& crypto)
    : transport_(transport), crypto_(crypto) {}

bool AapHandshaker::Run() {
    if (!DoVersionExchange()) return false;
    if (!DoSslHandshake())    return false;
    if (!SendAuthComplete())  return false;
    return true;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

bool AapHandshaker::ReadInto(std::vector<uint8_t>& buf) {
    auto chunk = transport_.Receive();
    if (chunk.empty()) return false;
    buf.insert(buf.end(), chunk.begin(), chunk.end());
    return true;
}

void AapHandshaker::WalkPackets(std::vector<uint8_t>& buf, const PacketVisitor& visitor) {
    size_t offset = 0;
    while (buf.size() - offset >= aap::HEADER_SIZE) {
        uint16_t payload_len = (buf[offset + 2] << 8) | buf[offset + 3];
        size_t   total_len   = aap::HEADER_SIZE + payload_len;
        if (buf.size() - offset < total_len) break;

        uint8_t  channel  = buf[offset];
        uint16_t msg_type = (buf[offset + 4] << 8) | buf[offset + 5];
        std::vector<uint8_t> payload(buf.begin() + offset + 6,
                                      buf.begin() + offset + total_len);
        offset += total_len;
        if (!visitor(channel, msg_type, payload)) break;
    }
    buf.erase(buf.begin(), buf.begin() + offset);
}

bool AapHandshaker::DrainUntil(std::vector<uint8_t>& buf, uint16_t target_msg_type,
                                std::vector<uint8_t>* out_payload) {
    bool found = false;
    WalkPackets(buf, [&](uint8_t /*ch*/, uint16_t type, const std::vector<uint8_t>& payload) -> bool {
        if (type == target_msg_type) {
            if (out_payload) *out_payload = payload;
            found = true;
            return false; // stop
        }
        leftover_.insert(leftover_.end(), payload.begin(), payload.end());
        return true; // continue
    });
    return found;
}

// ---------------------------------------------------------------------------
// Stage 1: version exchange
// ---------------------------------------------------------------------------

bool AapHandshaker::DoVersionExchange() {
    std::vector<uint8_t> version_payload = {0, 1, 0, 1};  // AAP v1.1
    auto packet = aap::Pack(aap::CH_CONTROL, aap::TYPE_VERSION_REQ, version_payload, 0x03);
    if (!transport_.Send(packet)) {
        AA_LOG_E() << "Failed to send version request";
        return false;
    }

    std::vector<uint8_t> buf;
    std::vector<uint8_t> resp_payload;
    while (true) {
        if (!ReadInto(buf)) {
            AA_LOG_E() << "Failed to receive version response (timeout/empty)";
            return false;
        }
        if (DrainUntil(buf, aap::TYPE_VERSION_RESP, &resp_payload)) {
            // remaining buf bytes belong to the next stage
            leftover_.insert(leftover_.end(), buf.begin(), buf.end());

            // Payload layout (big-endian, see aasdk handleVersionResponse):
            //   uint16 major | uint16 minor | int16 status
            // status follows shared MessageStatus enum: 0 = SUCCESS,
            // -1 = NO_COMPATIBLE_VERSION, etc.
            if (resp_payload.size() >= 6) {
                uint16_t major = (resp_payload[0] << 8) | resp_payload[1];
                uint16_t minor = (resp_payload[2] << 8) | resp_payload[3];
                int16_t  status = static_cast<int16_t>(
                    (resp_payload[4] << 8) | resp_payload[5]);
                AA_LOG_I() << "Version exchange complete — phone reports v"
                           << major << "." << minor << " status=" << status;
                if (status != 0) {
                    AA_LOG_E() << "Phone refused version (status=" << status
                               << "); aborting handshake";
                    return false;
                }
            } else {
                // Older firmwares may omit the status word; accept the
                // response without it but log so we notice.
                AA_LOG_W() << "Version response shorter than expected ("
                           << resp_payload.size() << " bytes), assuming OK";
            }
            return true;
        }
    }
}

// ---------------------------------------------------------------------------
// Stage 2: SSL handshake
// ---------------------------------------------------------------------------

bool AapHandshaker::DoSslHandshake() {
    // Brief delay after version exchange to let the phone finish SSL initialization.
    // On some devices, sending SSL ClientHello too early causes it to be ignored.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    crypto_.SetStrategy(std::make_shared<crypto::TlsCryptoStrategy>());

    std::vector<uint8_t> buf;
    // Seed any leftover bytes from version exchange
    buf.swap(leftover_);

    for (int attempt = 0; attempt < 20 && !crypto_.IsHandshakeComplete(); ++attempt) {
        AA_LOG_I() << "SSL handshake attempt (" << (attempt + 1) << "/20)...";

        auto out_data = crypto_.GetHandshakeData();
        if (!out_data.empty()) {
            auto pkt = aap::Pack(aap::CH_CONTROL, aap::TYPE_SSL_HANDSHAKE, out_data, 0x03);
            if (!transport_.Send(pkt)) {
                AA_LOG_E() << "Failed to send SSL handshake data";
                return false;
            }
        }

        if (crypto_.IsHandshakeComplete()) break;

        if (!ReadInto(buf)) continue;

        // Route SSL handshake packets to crypto; everything else goes to leftover_
        WalkPackets(buf, [&](uint8_t channel, uint16_t type, const std::vector<uint8_t>& payload) -> bool {
            if (channel == aap::CH_CONTROL && type == aap::TYPE_SSL_HANDSHAKE)
                crypto_.PutHandshakeData(payload);
            else
                leftover_.insert(leftover_.end(), payload.begin(), payload.end());
            return true;
        });
    }

    if (!crypto_.IsHandshakeComplete()) {
        AA_LOG_E() << "SSL handshake failed";
        return false;
    }

    // Any remaining bytes belong to the message stream
    leftover_.insert(leftover_.end(), buf.begin(), buf.end());
    AA_LOG_I() << "SSL handshake complete";
    return true;
}

// ---------------------------------------------------------------------------
// Stage 3: auth complete
// ---------------------------------------------------------------------------

bool AapHandshaker::SendAuthComplete() {
    aap_protobuf::service::control::message::AuthResponse auth;
    auth.set_status(0);  // OK

    std::vector<uint8_t> payload(auth.ByteSize());
    if (!auth.SerializeToArray(payload.data(), payload.size())) return false;

    auto packet = aap::Pack(aap::CH_CONTROL, aap::TYPE_SSL_AUTH_COMPLETE, payload, 0x03);
    return transport_.Send(packet);
}

} // namespace session
} // namespace aauto
