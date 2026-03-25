#pragma once

#include <string>

namespace aauto {
namespace crypto {

// Reference App (android-auto-headunit) 에서 추출한 하드코딩된 인증서와 키입니다.
// Android Auto 폰과의 상호 인증에 사용됩니다.
// 실제 값은 AapKeys.cpp 에 정의되어 있습니다.

extern const std::string AAP_CERTIFICATE;
extern const std::string AAP_PRIVATE_KEY;

} // namespace crypto
} // namespace aauto
