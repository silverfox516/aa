#define LOG_TAG "AA.CORE.CryptoManager"
#include "aauto/crypto/CryptoManager.hpp"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <iostream>
#include "aauto/utils/Logger.hpp"

#include "aauto/crypto/AapKeys.hpp"

namespace aauto {
namespace crypto {

// --- OpenSSL Global Initialization ---

static void EnsureOpenSslInitialized() {
    static bool openssl_init = false;
    if (!openssl_init) {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        openssl_init = true;
    }
}

// --- TlsCryptoStrategy Implementation ---

TlsCryptoStrategy::TlsCryptoStrategy()
    : ssl_ctx_(nullptr), ssl_(nullptr), read_bio_(nullptr), write_bio_(nullptr), handshake_done_(false) {
    EnsureOpenSslInitialized();

    // Configure TLS client context
    const SSL_METHOD* method = TLS_client_method();
    ssl_ctx_ = SSL_CTX_new(method);

    // Lock to TLS 1.2 as required by the Android Auto specification
    SSL_CTX_set_min_proto_version(ssl_ctx_, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ssl_ctx_, TLS1_2_VERSION);

    // Disable certificate verification (matches reference app NoCheckTrustManager policy)
    SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_NONE, nullptr);

    ssl_ = SSL_new(ssl_ctx_);

    // Apply certificate and private key extracted from the reference app
    BIO* cert_bio = BIO_new_mem_buf(AAP_CERTIFICATE.c_str(), -1);
    X509* cert = PEM_read_bio_X509(cert_bio, nullptr, nullptr, nullptr);
    if (cert) {
        SSL_use_certificate(ssl_, cert);
        X509_free(cert);
    }
    BIO_free(cert_bio);

    BIO* key_bio = BIO_new_mem_buf(AAP_PRIVATE_KEY.c_str(), -1);
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(key_bio, nullptr, nullptr, nullptr);
    if (pkey) {
        SSL_use_PrivateKey(ssl_, pkey);
        EVP_PKEY_free(pkey);
    }
    BIO_free(key_bio);
    
    // Memory BIOs bridge the SSL engine and the application (USB transport)
    read_bio_ = BIO_new(BIO_s_mem());   // network -> SSL input
    write_bio_ = BIO_new(BIO_s_mem());  // SSL output -> network
    SSL_set_bio(ssl_, read_bio_, write_bio_);

    // Set client mode
    SSL_set_connect_state(ssl_);
}

TlsCryptoStrategy::~TlsCryptoStrategy() {
    if (ssl_) SSL_free(ssl_);
    if (ssl_ctx_) SSL_CTX_free(ssl_ctx_);
}

bool TlsCryptoStrategy::IsHandshakeComplete() const { return handshake_done_ || SSL_is_init_finished(ssl_); }

std::vector<uint8_t> TlsCryptoStrategy::GetHandshakeData() {
    // Drive the handshake state machine
    int ret = SSL_do_handshake(ssl_);
    if (ret <= 0) {
        int err = SSL_get_error(ssl_, ret);
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
            CryptoManager::LogSslError("[Crypto] SSL handshake internal error (" + std::to_string(err) + ")");
        }
    } else {
        handshake_done_ = true;
    }

    // Drain pending output data (ClientHello etc.) from write_bio
    size_t pending = BIO_ctrl_pending(write_bio_);
    if (pending > 0) {
        std::vector<uint8_t> output(pending);
        int read_len = BIO_read(write_bio_, output.data(), static_cast<int>(pending));
        output.resize(read_len);
        return output;
    }
    return {};
}

void TlsCryptoStrategy::PutHandshakeData(const std::vector<uint8_t>& data) {
    // Feed received handshake data (ServerHello etc.) into read_bio
    BIO_write(read_bio_, data.data(), static_cast<int>(data.size()));
}

std::vector<uint8_t> TlsCryptoStrategy::Encrypt(const std::vector<uint8_t>& plain_data) {
    if (!IsHandshakeComplete()) return plain_data;

    // Write plaintext into SSL (encryption happens here)
    int written = SSL_write(ssl_, plain_data.data(), static_cast<int>(plain_data.size()));
    if (written <= 0) {
        CryptoManager::LogSslError("[Crypto] SSL encrypt failed");
        return {};
    }

    // Drain the ciphertext from write_bio
    size_t pending = BIO_ctrl_pending(write_bio_);
    std::vector<uint8_t> cipher(pending);
    BIO_read(write_bio_, cipher.data(), static_cast<int>(pending));
    return cipher;
}

std::vector<uint8_t> TlsCryptoStrategy::Decrypt(const std::vector<uint8_t>& cipher_data) {
    if (!IsHandshakeComplete()) return cipher_data;

    // Feed ciphertext into read_bio
    BIO_write(read_bio_, cipher_data.data(), static_cast<int>(cipher_data.size()));

    // Loop until SSL_read returns WANT_READ to support multiple TLS records.
    // Large video I-frames can span several 16KB TLS records.
    std::vector<uint8_t> result;
    const int kReadBuf = 65536; // max read per iteration (16KB TLS record + margin)
    std::vector<uint8_t> tmp(kReadBuf);

    while (true) {
        int read_len = SSL_read(ssl_, tmp.data(), kReadBuf);
        if (read_len > 0) {
            result.insert(result.end(), tmp.begin(), tmp.begin() + read_len);
            continue; // more data may be available
        }
        // read_len <= 0
        int err = SSL_get_error(ssl_, read_len);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            break; // no more data — normal exit
        }
        if (err == SSL_ERROR_ZERO_RETURN) {
            break; // peer closed connection
        }
        CryptoManager::LogSslError("[Crypto] SSL decrypt failed (" + std::to_string(err) + ")");
        break;
    }

    return result;
}

// --- CryptoManager Implementation ---

CryptoManager::CryptoManager(std::shared_ptr<ICryptoStrategy> strategy) : strategy_(std::move(strategy)) {}

void CryptoManager::SetStrategy(std::shared_ptr<ICryptoStrategy> strategy) {
    std::lock_guard<std::mutex> lock(mutex_);
    strategy_ = std::move(strategy);
}

bool CryptoManager::IsHandshakeComplete() const { return strategy_ ? strategy_->IsHandshakeComplete() : true; }

std::vector<uint8_t> CryptoManager::GetHandshakeData() {
    std::lock_guard<std::mutex> lock(mutex_);
    return strategy_ ? strategy_->GetHandshakeData() : std::vector<uint8_t>{};
}

void CryptoManager::PutHandshakeData(const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (strategy_) strategy_->PutHandshakeData(data);
}

std::vector<uint8_t> CryptoManager::EncryptData(const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    return strategy_ ? strategy_->Encrypt(data) : data;
}

std::vector<uint8_t> CryptoManager::DecryptData(const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    return strategy_ ? strategy_->Decrypt(data) : data;
}

void CryptoManager::LogSslError(const std::string& prefix) {
    unsigned long err = ERR_get_error();
    while (err != 0) {
        char err_buf[256];
        ERR_error_string_n(err, err_buf, sizeof(err_buf));
        AA_LOG_E() << prefix << ": " << err_buf;
        err = ERR_get_error();
    }
}

}  // namespace crypto
}  // namespace aauto
