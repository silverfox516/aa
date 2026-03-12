#pragma once

#include <string>

namespace aauto {
namespace core {

// All headunit identity and capability parameters that are sent to the phone
// during service discovery. Centralizes what was previously scattered across
// ControlService, VideoService, InputService, and ServiceFactory.
struct HeadunitConfig {
    // Vehicle / head unit identity (sent in ServiceDiscoveryResponse)
    std::string vehicle_make             = "Google";
    std::string vehicle_model            = "PixelSim";
    std::string vehicle_year             = "2026";
    std::string vehicle_id               = "VIN1234567890AA";
    std::string head_unit_make           = "OpenSource";
    std::string head_unit_model          = "AAuto";
    std::string head_unit_software_build = "1.0.0";
    std::string head_unit_software_version = "1.0";

    // Display capabilities (sent in VideoService / InputService definitions)
    int display_width   = 1280;
    int display_height  = 720;
    int display_density = 140;  // dpi
};

} // namespace core
} // namespace aauto
