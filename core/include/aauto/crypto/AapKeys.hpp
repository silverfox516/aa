#pragma once

#include <string>

namespace aauto {
namespace crypto {

// Hardcoded certificate and private key extracted from the reference app (android-auto-headunit).
// Used for mutual authentication with the Android Auto phone.
// Actual values are defined in AapKeys.cpp.

extern const std::string AAP_CERTIFICATE;
extern const std::string AAP_PRIVATE_KEY;

} // namespace crypto
} // namespace aauto
