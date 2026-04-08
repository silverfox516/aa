#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

// OpenSSL forward declarations
struct ssl_ctx_st; typedef struct ssl_ctx_st SSL_CTX;
struct ssl_st;     typedef struct ssl_st SSL;
struct bio_st;     typedef struct bio_st BIO;

namespace aauto {
namespace crypto {

class ICryptoStrategy {
   public:
    virtual ~ICryptoStrategy() = default;

    virtual bool IsHandshakeComplete() const { return true; }
    virtual std::vector<uint8_t> GetHandshakeData() { return {}; }
    virtual void PutHandshakeData(const std::vector<uint8_t>& data) {}

    virtual std::vector<uint8_t> Encrypt(const std::vector<uint8_t>& plain_data) = 0;

    // Decrypt one fragment of ciphertext, appending any complete plaintext
    // bytes to `output`. The TLS state is preserved between calls so partial
    // records are tolerated: a successful call may append zero bytes when the
    // current record is not yet complete. Returns false only on a fatal SSL
    // error (BAD_DECRYPT, MAC mismatch, etc).
    virtual bool Decrypt(const std::vector<uint8_t>& cipher_data,
                          std::vector<uint8_t>& output) = 0;
};

class TlsCryptoStrategy : public ICryptoStrategy {
   public:
    TlsCryptoStrategy();
    ~TlsCryptoStrategy() override;

    bool IsHandshakeComplete() const override;
    std::vector<uint8_t> GetHandshakeData() override;
    void PutHandshakeData(const std::vector<uint8_t>& data) override;
    std::vector<uint8_t> Encrypt(const std::vector<uint8_t>& plain_data) override;
    bool Decrypt(const std::vector<uint8_t>& cipher_data,
                  std::vector<uint8_t>& output) override;

   private:
    SSL_CTX* ssl_ctx_;
    SSL*     ssl_;
    BIO*     read_bio_;   // Network -> SSL
    BIO*     write_bio_;  // SSL -> Network
    bool     handshake_done_;
};

class CryptoManager {
   public:
    explicit CryptoManager(std::shared_ptr<ICryptoStrategy> strategy);

    void SetStrategy(std::shared_ptr<ICryptoStrategy> strategy);

    bool IsHandshakeComplete() const;
    std::vector<uint8_t> GetHandshakeData();
    void PutHandshakeData(const std::vector<uint8_t>& data);

    std::vector<uint8_t> EncryptData(const std::vector<uint8_t>& data);

    // Decrypt one fragment, appending plaintext to `output`. See
    // ICryptoStrategy::Decrypt for partial-record semantics.
    bool DecryptData(const std::vector<uint8_t>& cipher,
                      std::vector<uint8_t>& output);

    static void LogSslError(const std::string& prefix);

   private:
    std::shared_ptr<ICryptoStrategy> strategy_;
    mutable std::mutex mutex_;
};

} // namespace crypto
} // namespace aauto
