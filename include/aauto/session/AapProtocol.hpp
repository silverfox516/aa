#pragma once

#include <cstdint>
#include <vector>

namespace aauto {
namespace session {

namespace aap {

static constexpr uint8_t HEADER_SIZE = 4;
static constexpr uint8_t TYPE_SIZE = 2;

// Channels
static constexpr uint8_t CH_CONTROL = 0;

// Message Types
static constexpr uint16_t TYPE_VERSION_REQ = 1;
static constexpr uint16_t TYPE_VERSION_RESP = 2;
static constexpr uint16_t TYPE_SSL_HANDSHAKE = 3;
static constexpr uint16_t TYPE_SSL_AUTH_COMPLETE = 4;
static constexpr uint16_t TYPE_SERVICE_DISCOVERY_REQ = 5;
static constexpr uint16_t TYPE_SERVICE_DISCOVERY_RESP = 6;

// Flags
static constexpr uint8_t FLAG_FIRST = 0x10;
static constexpr uint8_t FLAG_LAST = 0x20;
static constexpr uint8_t FLAG_CONTROL = 0x08; // Encrypted or Control? Reference says 0x08 is control-ish in some versions

// Helper to create a raw AAP packet
inline std::vector<uint8_t> Pack(uint8_t channel, uint16_t type, const std::vector<uint8_t>& payload) {
    uint16_t total_len = static_cast<uint16_t>(payload.size() + TYPE_SIZE);
    std::vector<uint8_t> packet(HEADER_SIZE + TYPE_SIZE + payload.size());
    
    packet[0] = channel;
    packet[1] = 3; // Version 1.x flag? Reference uses decimal 3 (binary 11) for Hanshake/Version.
    packet[2] = (total_len >> 8) & 0xFF;
    packet[3] = total_len & 0xFF;
    
    packet[4] = (type >> 8) & 0xFF;
    packet[5] = type & 0xFF;
    
    if (!payload.empty()) {
        std::copy(payload.begin(), payload.end(), packet.begin() + 6);
    }
    
    return packet;
}

} // namespace aap

} // namespace session
} // namespace aauto
