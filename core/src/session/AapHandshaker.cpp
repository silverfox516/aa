#define LOG_TAG "AapHandshaker"
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

// Drains complete AAP packets from buf.
// - If a packet matches target_msg_type: stores its payload in *out_payload (if provided),
//   removes it from buf, and returns true.
// - Any other complete packet is moved to leftover_.
// - Incomplete packets are left in buf.
bool AapHandshaker::DrainUntil(std::vector<uint8_t>& buf, uint16_t target_msg_type,
                                std::vector<uint8_t>* out_payload) {
    size_t offset = 0;
    while (buf.size() - offset >= aap::HEADER_SIZE) {
        uint16_t payload_len    = (buf[offset + 2] << 8) | buf[offset + 3];
        size_t   total_len      = aap::HEADER_SIZE + payload_len;
        if (buf.size() - offset < total_len) break;

        uint16_t msg_type = (buf[offset + 4] << 8) | buf[offset + 5];

        if (msg_type == target_msg_type) {
            if (out_payload) {
                *out_payload = std::vector<uint8_t>(buf.begin() + offset + 6,
                                                    buf.begin() + offset + total_len);
            }
            buf.erase(buf.begin() + offset, buf.begin() + offset + total_len);
            return true;
        }

        // Not our target — keep for ProcessLoop
        leftover_.insert(leftover_.end(),
                         buf.begin() + offset, buf.begin() + offset + total_len);
        offset += total_len;
    }
    buf.erase(buf.begin(), buf.begin() + offset);
    return false;
}

// ---------------------------------------------------------------------------
// Stage 1: version exchange
// ---------------------------------------------------------------------------

bool AapHandshaker::DoVersionExchange() {
    std::vector<uint8_t> version_payload = {0, 1, 0, 1};  // AAP v1.1
    auto packet = aap::Pack(aap::CH_CONTROL, aap::TYPE_VERSION_REQ, version_payload, 0x03);
    if (!transport_.Send(packet)) {
        AA_LOG_E() << "버전 요청 송신 실패";
        return false;
    }

    std::vector<uint8_t> buf;
    while (true) {
        if (!ReadInto(buf)) {
            AA_LOG_E() << "버전 응답 수신 실패 (Timeout/Empty)";
            return false;
        }
        if (DrainUntil(buf, aap::TYPE_VERSION_RESP)) {
            // remaining buf bytes belong to the next stage
            leftover_.insert(leftover_.end(), buf.begin(), buf.end());
            AA_LOG_I() << "버전 교환 완료";
            return true;
        }
    }
}

// ---------------------------------------------------------------------------
// Stage 2: SSL handshake
// ---------------------------------------------------------------------------

bool AapHandshaker::DoSslHandshake() {
    // 버전 교환 직후 폰이 SSL 초기화를 완료할 때까지 짧게 대기.
    // 일부 기기에서 너무 빨리 SSL ClientHello를 보내면 무시되는 현상이 있어 삽입.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    crypto_.SetStrategy(std::make_shared<crypto::TlsCryptoStrategy>());

    std::vector<uint8_t> buf;
    // Seed any leftover bytes from version exchange
    buf.swap(leftover_);

    for (int attempt = 0; attempt < 20 && !crypto_.IsHandshakeComplete(); ++attempt) {
        AA_LOG_I() << "SSL 핸드셰이크 시도 (" << (attempt + 1) << "/20)...";

        auto out_data = crypto_.GetHandshakeData();
        if (!out_data.empty()) {
            auto pkt = aap::Pack(aap::CH_CONTROL, aap::TYPE_SSL_HANDSHAKE, out_data, 0x03);
            if (!transport_.Send(pkt)) {
                AA_LOG_E() << "SSL 핸드셰이크 데이터 송신 실패";
                return false;
            }
        }

        if (crypto_.IsHandshakeComplete()) break;

        if (!ReadInto(buf)) continue;

        // Extract all SSL handshake packets; others go to leftover_
        size_t offset = 0;
        while (buf.size() - offset >= aap::HEADER_SIZE) {
            uint16_t payload_len = (buf[offset + 2] << 8) | buf[offset + 3];
            size_t   total_len   = aap::HEADER_SIZE + payload_len;
            if (buf.size() - offset < total_len) break;

            uint8_t  channel  = buf[offset];
            uint16_t msg_type = (buf[offset + 4] << 8) | buf[offset + 5];

            if (channel == aap::CH_CONTROL && msg_type == aap::TYPE_SSL_HANDSHAKE) {
                std::vector<uint8_t> ssl_payload(buf.begin() + offset + 6,
                                                  buf.begin() + offset + total_len);
                crypto_.PutHandshakeData(ssl_payload);
            } else {
                leftover_.insert(leftover_.end(),
                                  buf.begin() + offset, buf.begin() + offset + total_len);
            }
            offset += total_len;
        }
        buf.erase(buf.begin(), buf.begin() + offset);
    }

    if (!crypto_.IsHandshakeComplete()) {
        AA_LOG_E() << "SSL 핸드셰이크 실패";
        return false;
    }

    // Any remaining bytes belong to the message stream
    leftover_.insert(leftover_.end(), buf.begin(), buf.end());
    AA_LOG_I() << "SSL 핸드셰이크 완료!";
    return true;
}

// ---------------------------------------------------------------------------
// Stage 3: auth complete
// ---------------------------------------------------------------------------

bool AapHandshaker::SendAuthComplete() {
    aap_protobuf::service::control::message::AuthResponse auth;
    auth.set_status(0);  // OK

    std::vector<uint8_t> payload(auth.ByteSizeLong());
    if (!auth.SerializeToArray(payload.data(), payload.size())) return false;

    auto packet = aap::Pack(aap::CH_CONTROL, aap::TYPE_SSL_AUTH_COMPLETE, payload, 0x03);
    return transport_.Send(packet);
}

} // namespace session
} // namespace aauto
