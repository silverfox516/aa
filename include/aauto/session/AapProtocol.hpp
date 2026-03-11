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

// --- Flags (byte 1 of AAP header) ---
// Reference: AapMessage.kt flags(channel, type)
//   default = 0x0b
//   if (channel != CONTROL && isControl(type)):  flags = 0x0f
//
// isControl(type): type is a "session-layer" message type (0x0001 ~ 0x0013)
//   These are: version, ssl, service-discovery, channel-open, ping, nav/audio-focus...
//
// In practice:
//   0x08 = Encrypted flag (set by send_cb after this function)
//   0x03 = some packet framing bits always present
//   0x04 = "control message on a media channel" flag

static constexpr uint8_t FLAGS_DEFAULT         = 0x0b; // 0x03 | 0x08 (encrypted)
static constexpr uint8_t FLAGS_CONTROL_ON_MEDIA = 0x0f; // 0x07 | 0x08 (control msg on media ch)

// Range of "session control" message types (from proto MEDIADATA .. AUDIOFOCUSNOTFICATION)
// MEDIADATA = 0x8000+, but the lower range 0x0001~0x0013 covers all handshake + channel msgs.
// isControl from Kotlin: type >= MEDIADATA.number && type <= AUDIOFOCUSNOTFICATION.number
// AUDIOFOCUSNOTFICATION = 0x0013 (19 decimal)
// MEDIADATA (media data sent over service channels) is a high value like 0x8001.
// So the "control" range from that check essentially means: 0x0001 <= type <= 0x0013
inline bool IsControlType(uint16_t type) {
    return type >= 0x0001 && type <= 0x0013;
}

// Compute flags the same way AapMessage.kt does:
//   - default: 0x0b
//   - non-control channel + control-type message: 0x0f
// Note: the encrypted bit (0x08) is already included in both.
// For pre-SSL handshake packets the send path does NOT call this function — it
// sends via transport directly without the encrypted bits below anyway; but those
// packets are also sent on channel 0 where we never flip to 0x0f.
inline uint8_t ComputeFlags(uint8_t channel, uint16_t type) {
    if (channel != CH_CONTROL && IsControlType(type)) {
        return FLAGS_CONTROL_ON_MEDIA; // 0x0f
    }
    return FLAGS_DEFAULT; // 0x0b
}

// Helper to create a raw AAP packet.
// - flags is auto-computed from (channel, type) by default (post-handshake encrypted msgs).
// - Pre-SSL handshake callers (version exchange, SSL data) must pass flags=0x03 explicitly
//   because those packets are NOT encrypted and 0x08 must NOT be set.
inline std::vector<uint8_t> Pack(uint8_t channel, uint16_t type, const std::vector<uint8_t>& payload,
                                  uint8_t flags = 0xFF /* 0xFF = auto-compute */) {
    if (flags == 0xFF) {
        flags = ComputeFlags(channel, type);
    }
    uint16_t total_len = static_cast<uint16_t>(payload.size() + TYPE_SIZE);
    std::vector<uint8_t> packet(HEADER_SIZE + TYPE_SIZE + payload.size());

    packet[0] = channel;
    packet[1] = flags;
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
