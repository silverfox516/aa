#include "aauto/crypto/CryptoManager.hpp"

#include <iostream>

namespace aauto {
namespace crypto {

std::vector<uint8_t> AesCryptoStrategy::Encrypt(const std::vector<uint8_t>& plain_data) {
    // TODO: 실제 AES 암호화 로직 구현 구역
    std::cout << "[Crypto] 데이터를 AES로 암호화합니다." << std::endl;
    return plain_data;  // 현재는 원본 반환
}

std::vector<uint8_t> AesCryptoStrategy::Decrypt(const std::vector<uint8_t>& cipher_data) {
    // TODO: 실제 AES 복호화 로직 구현 구역
    std::cout << "[Crypto] 데이터를 AES로 복호화합니다." << std::endl;
    return cipher_data;  // 현재는 원본 반환
}

CryptoManager::CryptoManager(std::shared_ptr<ICryptoStrategy> strategy) : strategy_(std::move(strategy)) {}

void CryptoManager::SetStrategy(std::shared_ptr<ICryptoStrategy> strategy) { strategy_ = std::move(strategy); }

std::vector<uint8_t> CryptoManager::EncryptData(const std::vector<uint8_t>& data) {
    if (strategy_) {
        return strategy_->Encrypt(data);
    }
    return data;  // 암호화 전략이 없으면 평문 반환
}

std::vector<uint8_t> CryptoManager::DecryptData(const std::vector<uint8_t>& data) {
    if (strategy_) {
        return strategy_->Decrypt(data);
    }
    return data;
}

}  // namespace crypto
}  // namespace aauto
