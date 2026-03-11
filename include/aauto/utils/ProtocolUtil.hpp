#pragma once

#include <string>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace aauto {
namespace utils {

class ProtocolUtil {
public:
    static std::string GetChannelName(uint8_t ch) {
        if (ch == 0) return "Control";
        if (ch >= 1 && ch <= 3) return "Audio"; // Media, Guidance, System
        if (ch == 4) return "Video";
        if (ch == 5) return "Input";
        if (ch == 6) return "Sensor";
        if (ch == 7) return "Mic";
        if (ch == 255) return "Bluetooth";
        return "Unknown(" + std::to_string(ch) + ")";
    }

    static std::string GetMessageTypeName(uint16_t type) {
        switch (type) {
            // AAP Common Types
            case 0x0001: return "VersionRequest";
            case 0x0002: return "VersionResponse";
            case 0x0003: return "SslHandshake";
            case 0x0004: return "SslAuthComplete";
            case 0x0005: return "ServiceDiscoveryRequest";
            case 0x0006: return "ServiceDiscoveryResponse";
            case 0x0007: return "ChannelOpenRequest";
            case 0x0008: return "ChannelOpenResponse";
            case 0x000B: return "PingRequest";
            case 0x000C: return "PingResponse";
            case 0x000D: return "NavFocusRequestNotification";
            case 0x000E: return "NavFocusNotification";
            case 0x0012: return "AudioFocusRequestNotification";
            case 0x0013: return "AudioFocusNotification";

            // Service Specific Types (ORed with 0x8000 often)
            case 0x8000: return "MediaSetupRequest";
            case 0x8001: return "MediaStart/SensorRequest";
            case 0x8002: return "MediaStop";
            case 0x8003: return "Config/SensorEvent";
            case 0x8007: return "VideoFocusRequestNotification";
            case 0x8008: return "VideoFocusNotification";
            
            default: {
                char buf[16];
                snprintf(buf, sizeof(buf), "0x%04X", type);
                return std::string(buf);
            }
        }
    }

    static std::string DumpHex(const std::vector<uint8_t>& data, size_t max_len = 16) {
        if (data.empty()) return "()";
        
        std::ostringstream oss;
        oss << "(" << data.size() << " bytes): ";
        
        size_t len = (max_len == 0) ? data.size() : std::min(data.size(), max_len);
        for (size_t i = 0; i < len; ++i) {
            oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]) << " ";
        }
        
        if (max_len != 0 && data.size() > max_len) {
            oss << "...";
        }
        
        return oss.str();
    }
};

} // namespace utils
} // namespace aauto
