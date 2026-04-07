#pragma once

#include <string>

namespace aauto {
namespace session {

// Identification information about a connected phone, extracted from the
// AAP ServiceDiscoveryRequest. Forwarded by ControlService through the
// session callback so the app layer can populate its session entry and
// look up the persistent device profile.
//
// Identifier semantics (per AAP spec):
//   - instance_id: persistent UUID for the phone, stable across reconnects.
//                  Use as the key for persistent device preferences.
//   - connectivity_lifetime_id: per-connection session ID issued by the
//                  phone. Changes on every reconnect. Use as a log
//                  correlation label, NOT as a session-manager key
//                  (collisions are possible across phones).
struct PhoneInfo {
    std::string instance_id;
    std::string connectivity_lifetime_id;
    std::string device_name;     // ServiceDiscoveryRequest.device_name
    std::string label_text;      // ServiceDiscoveryRequest.label_text
};

} // namespace session
} // namespace aauto
