#pragma once

#include <memory>
#include <string>  // Changed from <cstdint>
#include <vector>

namespace aauto {
namespace crypto {

// 암호화/복호화 알고리즘의 전략을 정의하는 인터페이스
class ICryptoStrategy {
   public:
    virtual ~ICryptoStrategy() = default;

    // 암호화 수행
    virtual std::vector<uint8_t> Encrypt(const std::vector<uint8_t>& plain_data) = 0;

    // 복호화 수행
    virtual std::vector<uint8_t> Decrypt(const std::vector<uint8_t>& cipher_data) = 0;
};

// 실제 적용될 예시: AES 암호화 전략 (Placeholder)
class AesCryptoStrategy : public ICryptoStrategy {
   public:
    std::vector<uint8_t> Encrypt(const std::vector<uint8_t>& plain_data) override;
    std::vector<uint8_t> Decrypt(const std::vector<uint8_t>& cipher_data) override;
};

// 암호화 컨텍스트를 관리하는 클래스 (세션마다 고유하게 가짐)
class CryptoManager {
   public:
    explicit CryptoManager(std::shared_ptr<ICryptoStrategy> strategy);

    // 런타임에 암호화 알고리즘 교체 가능 (Strategy Pattern 장점)
    void SetStrategy(std::shared_ptr<ICryptoStrategy> strategy);

    std::vector<uint8_t> EncryptData(const std::vector<uint8_t>& data);
    std::vector<uint8_t> DecryptData(const std::vector<uint8_t>& data);

   private:
    std::shared_ptr<ICryptoStrategy> strategy_;
};

}  // namespace crypto
}  // namespace aauto
