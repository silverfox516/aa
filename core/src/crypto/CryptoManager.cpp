#define LOG_TAG "AA.CryptoManager"
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

    // TLS 클라이언트 환경 설정 (최신 OpenSSL 방식)
    const SSL_METHOD* method = TLS_client_method();
    ssl_ctx_ = SSL_CTX_new(method);

    // TLS 1.2로 버전 고정 (Android Auto 규격)
    SSL_CTX_set_min_proto_version(ssl_ctx_, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ssl_ctx_, TLS1_2_VERSION);

    // 인증서 검증 비활성화 (레퍼런스 앱 NoCheckTrustManager 정책 따름)
    SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_NONE, nullptr);

    ssl_ = SSL_new(ssl_ctx_);

    // 레퍼런스 앱에서 추출한 인증서와 키 적용
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
    
    // Memory BIO 생성: SSL 엔진과 어플리케이션(USB Transport) 사이의 가교
    read_bio_ = BIO_new(BIO_s_mem());   // 네트워크에서 받은 데이터 -> SSL 로 입력
    write_bio_ = BIO_new(BIO_s_mem());  // SSL 에서 처리한 데이터 -> 네트워크로 전송
    SSL_set_bio(ssl_, read_bio_, write_bio_);

    // 클라이언트 모드로 설정
    SSL_set_connect_state(ssl_);
}

TlsCryptoStrategy::~TlsCryptoStrategy() {
    if (ssl_) SSL_free(ssl_);
    if (ssl_ctx_) SSL_CTX_free(ssl_ctx_);
}

bool TlsCryptoStrategy::IsHandshakeComplete() const { return handshake_done_ || SSL_is_init_finished(ssl_); }

std::vector<uint8_t> TlsCryptoStrategy::GetHandshakeData() {
    // SSL_do_handshake()를 호출하여 핸드셰이크 진행
    int ret = SSL_do_handshake(ssl_);
    if (ret <= 0) {
        int err = SSL_get_error(ssl_, ret);
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
            CryptoManager::LogSslError("[Crypto] SSL 핸드셰이크 내부 오류 (" + std::to_string(err) + ")");
        }
    } else {
        handshake_done_ = true;
    }

    // write_bio에 쌓인 데이터(ClientHello 등)를 꺼내서 반환
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
    // 네트워크에서 받은 핸드셰이크 데이터(ServerHello 등)를 read_bio에 주입
    BIO_write(read_bio_, data.data(), static_cast<int>(data.size()));
}

std::vector<uint8_t> TlsCryptoStrategy::Encrypt(const std::vector<uint8_t>& plain_data) {
    if (!IsHandshakeComplete()) return plain_data;

    // 데이터를 SSL로 쓰기 (암호화 발생)
    int written = SSL_write(ssl_, plain_data.data(), static_cast<int>(plain_data.size()));
    if (written <= 0) {
        CryptoManager::LogSslError("[Crypto] SSL 암호화 실패");
        return {};
    }

    // 암호화된 데이터를 write_bio에서 꺼내기
    size_t pending = BIO_ctrl_pending(write_bio_);
    std::vector<uint8_t> cipher(pending);
    BIO_read(write_bio_, cipher.data(), static_cast<int>(pending));
    return cipher;
}

std::vector<uint8_t> TlsCryptoStrategy::Decrypt(const std::vector<uint8_t>& cipher_data) {
    if (!IsHandshakeComplete()) return cipher_data;

    // 암호화된 데이터를 read_bio에 주입
    BIO_write(read_bio_, cipher_data.data(), static_cast<int>(cipher_data.size()));

    // SSL_read가 WANT_READ를 반환할 때까지 루프 (여러 TLS 레코드 지원)
    // 대형 비디오 I-프레임은 16KB 단위 TLS 레코드 여러 개로 나뉠 수 있음
    std::vector<uint8_t> result;
    const int kReadBuf = 65536; // 한 번에 읽을 최대 크기 (TLS 레코드 최대 16KB + 마진)
    std::vector<uint8_t> tmp(kReadBuf);

    while (true) {
        int read_len = SSL_read(ssl_, tmp.data(), kReadBuf);
        if (read_len > 0) {
            result.insert(result.end(), tmp.begin(), tmp.begin() + read_len);
            continue; // 더 읽을 데이터가 있을 수 있음
        }
        // read_len <= 0
        int err = SSL_get_error(ssl_, read_len);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            break; // 더 이상 읽을 데이터 없음 - 정상
        }
        if (err == SSL_ERROR_ZERO_RETURN) {
            break; // 상대방이 연결 종료
        }
        CryptoManager::LogSslError("[Crypto] SSL 복호화 실패 (" + std::to_string(err) + ")");
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
