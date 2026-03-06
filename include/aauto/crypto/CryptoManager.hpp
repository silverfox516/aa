#pragma once

#include <memory>
#include <string>  // Changed from <cstdint>
#include <vector>

// OpenSSL forward declarations
struct ssl_ctx_st;
typedef struct ssl_ctx_st SSL_CTX;
struct ssl_st;
typedef struct ssl_st SSL;
struct bio_st;
typedef struct bio_st BIO;

namespace aauto {
namespace crypto {

// 암호화/복호화 알고리즘의 전략을 정의하는 인터페이스
class ICryptoStrategy {
   public:
    virtual ~ICryptoStrategy() = default;

    // 핸드셰이크 관련
    virtual bool IsHandshakeComplete() const { return true; }
    virtual std::vector<uint8_t> GetHandshakeData() { return {}; }
    virtual void PutHandshakeData(const std::vector<uint8_t>& data) {}

    // 암호화 수행
    virtual std::vector<uint8_t> Encrypt(const std::vector<uint8_t>& plain_data) = 0;

    // 복호화 수행
    virtual std::vector<uint8_t> Decrypt(const std::vector<uint8_t>& cipher_data) = 0;
};

// Android Auto용 TLS 1.2 암호화 전략
class TlsCryptoStrategy : public ICryptoStrategy {
   public:
    TlsCryptoStrategy();
    ~TlsCryptoStrategy() override;

    bool IsHandshakeComplete() const override;
    std::vector<uint8_t> GetHandshakeData() override;
    void PutHandshakeData(const std::vector<uint8_t>& data) override;

    std::vector<uint8_t> Encrypt(const std::vector<uint8_t>& plain_data) override;
    std::vector<uint8_t> Decrypt(const std::vector<uint8_t>& cipher_data) override;

   private:
    SSL_CTX* ssl_ctx_;
    SSL* ssl_;
    BIO* read_bio_;   // Network -> SSL
    BIO* write_bio_;  // SSL -> Network
    bool handshake_done_;
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

    bool IsHandshakeComplete() const;
    std::vector<uint8_t> GetHandshakeData();
    void PutHandshakeData(const std::vector<uint8_t>& data);

    std::vector<uint8_t> EncryptData(const std::vector<uint8_t>& data);
    std::vector<uint8_t> DecryptData(const std::vector<uint8_t>& data);

    // OpenSSL 에러 출력을 위한 유틸리티
    static void LogSslError(const std::string& prefix);

   private:
    std::shared_ptr<ICryptoStrategy> strategy_;
};

}  // namespace crypto
}  // namespace aauto
