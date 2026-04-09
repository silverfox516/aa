#pragma once

#include <cstdint>
#include <vector>

namespace aauto {
namespace session {

namespace aap {

static constexpr uint8_t HEADER_SIZE = 4;
static constexpr uint8_t TYPE_SIZE   = 2;

// Channels
static constexpr uint8_t CH_CONTROL   = 0;
static constexpr uint8_t CH_BLUETOOTH = 0xFF;

// --- Flags (byte 1 of AAP header) ---
static constexpr uint8_t FLAGS_DEFAULT          = 0x0b; // first|last|encrypted
static constexpr uint8_t FLAGS_CONTROL_ON_MEDIA = 0x0f; // first|last|encrypted|control
static constexpr uint8_t FLAG_ENCRYPTED         = 0x08;
static constexpr uint8_t FLAG_FIRST             = 0x01;
static constexpr uint8_t FLAG_LAST              = 0x02;

// isControl: type 0x0001–0x0013 is a session-layer control type
inline bool IsControlType(uint16_t type) {
    return type >= 0x0001 && type <= 0x0013;
}

inline uint8_t ComputeFlags(uint8_t channel, uint16_t type) {
    if (channel != CH_CONTROL && IsControlType(type)) {
        return FLAGS_CONTROL_ON_MEDIA;
    }
    return FLAGS_DEFAULT;
}

// Build a raw AAP packet.
// Pass flags=0xFF to auto-compute (post-handshake encrypted messages).
// Pass flags=0x03 explicitly for pre-SSL handshake packets (not encrypted).
inline std::vector<uint8_t> Pack(uint8_t channel, uint16_t type, const std::vector<uint8_t>& payload,
                                  uint8_t flags = 0xFF) {
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

// --- All protocol message type constants ---
namespace msg {

// Session handshake & control (channel 0)
static constexpr uint16_t VERSION_REQ            = 0x0001;
static constexpr uint16_t VERSION_RESP           = 0x0002;
static constexpr uint16_t SSL_HANDSHAKE          = 0x0003;
static constexpr uint16_t SSL_AUTH_COMPLETE      = 0x0004;
static constexpr uint16_t SERVICE_DISCOVERY_REQ  = 0x0005;
static constexpr uint16_t SERVICE_DISCOVERY_RESP = 0x0006;
static constexpr uint16_t CHANNEL_OPEN_REQUEST   = 0x0007;
static constexpr uint16_t CHANNEL_OPEN_RESPONSE  = 0x0008;
static constexpr uint16_t PING_REQUEST           = 0x000B;
static constexpr uint16_t PING_RESPONSE          = 0x000C;
static constexpr uint16_t NAV_FOCUS_REQUEST      = 0x000D;
static constexpr uint16_t NAV_FOCUS_NOTIFICATION = 0x000E;
static constexpr uint16_t BYEBYE_REQUEST         = 0x000F;
static constexpr uint16_t BYEBYE_RESPONSE        = 0x0010;
static constexpr uint16_t AUDIO_FOCUS_REQUEST    = 0x0012;
static constexpr uint16_t AUDIO_FOCUS_NOTIFICATION = 0x0013;

// Media service (audio, video, mic)
static constexpr uint16_t MEDIA_DATA             = 0x0000;
static constexpr uint16_t MEDIA_CODEC_CONFIG     = 0x0001;
static constexpr uint16_t MEDIA_SETUP            = 0x8000;
static constexpr uint16_t MEDIA_START            = 0x8001;
static constexpr uint16_t MEDIA_STOP             = 0x8002;
static constexpr uint16_t MEDIA_CONFIG           = 0x8003;
static constexpr uint16_t MEDIA_ACK              = 0x8004;
static constexpr uint16_t VIDEO_FOCUS_REQUEST    = 0x8007;
static constexpr uint16_t VIDEO_FOCUS_NOTIFICATION = 0x8008;
static constexpr uint16_t MIC_REQUEST            = 0x8005;
static constexpr uint16_t MIC_RESPONSE           = 0x8006;

// Input service
static constexpr uint16_t INPUT_EVENT            = 0x8001;
static constexpr uint16_t INPUT_BINDING_REQUEST  = 0x8002;
static constexpr uint16_t INPUT_BINDING_RESPONSE = 0x8003;

// Sensor service
static constexpr uint16_t SENSOR_START_REQUEST   = 0x8001;
static constexpr uint16_t SENSOR_START_RESPONSE  = 0x8002;
static constexpr uint16_t SENSOR_EVENT           = 0x8003;

} // namespace msg

// Legacy aliases — keep existing callers compiling without change
static constexpr uint16_t TYPE_VERSION_REQ            = msg::VERSION_REQ;
static constexpr uint16_t TYPE_VERSION_RESP           = msg::VERSION_RESP;
static constexpr uint16_t TYPE_SSL_HANDSHAKE          = msg::SSL_HANDSHAKE;
static constexpr uint16_t TYPE_SSL_AUTH_COMPLETE      = msg::SSL_AUTH_COMPLETE;
static constexpr uint16_t TYPE_SERVICE_DISCOVERY_REQ  = msg::SERVICE_DISCOVERY_REQ;
static constexpr uint16_t TYPE_SERVICE_DISCOVERY_RESP = msg::SERVICE_DISCOVERY_RESP;

} // namespace aap

} // namespace session
} // namespace aauto
