#define LOG_TAG "Session"
#include "aauto/session/Session.hpp"
#include "aauto/session/AapProtocol.hpp"

#include <iomanip>

#include "aauto/utils/Logger.hpp"

namespace aauto {
namespace session {

Session::Session(std::shared_ptr<transport::ITransport> transport, std::shared_ptr<crypto::CryptoManager> crypto)
    : transport_(std::move(transport)), crypto_(std::move(crypto)), state_(SessionState::DISCONNECTED) {}

Session::~Session() { Stop(); }

void Session::RegisterService(std::shared_ptr<service::IService> service) {
    if (!service) return;

    std::unique_lock<std::shared_mutex> lock(services_mutex_);
    services_[service->GetType()] = service;
    AA_LOG_I() << "서비스 등록됨: " << service->GetName();
}

bool Session::Start() {
    if (!transport_ || !crypto_) return false;

    if (!transport_->Connect({})) {
        AA_LOG_E() << "Transport 연결 실패";
        return false;
    }

    // 상태 전환: DISCONNECTED -> HANDSHAKE
    SessionState expected = SessionState::DISCONNECTED;
    if (!state_.compare_exchange_strong(expected, SessionState::HANDSHAKE)) {
        return false;
    }

    AA_LOG_I() << "핸드셰이크를 시작합니다...";

    // 1단계: 버전 교환
    if (!DoVersionExchange()) {
        state_.store(SessionState::DISCONNECTED);
        return false;
    }

    // 2단계: SSL 핸드셰이크
    if (!DoSslHandshake()) {
        state_.store(SessionState::DISCONNECTED);
        return false;
    }

    // 3단계: 인증 완료 알림
    if (!SendSslAuthComplete()) {
        state_.store(SessionState::DISCONNECTED);
        return false;
    }

    state_.store(SessionState::CONNECTED);
    AA_LOG_I() << "세션이 정상적으로 연결되었습니다 (CONNECTED).";

    // 수신 및 처리 루프 시작
    receive_thread_ = std::thread(&Session::ReceiveLoop, this);
    process_thread_ = std::thread(&Session::ProcessLoop, this);

    return true;
}

bool Session::DoVersionExchange() {
    std::vector<uint8_t> version_payload = {0, 1, 0, 1};  // AAP v1.1
    auto version_packet = aap::Pack(aap::CH_CONTROL, aap::TYPE_VERSION_REQ, version_payload);

    if (!transport_->Send(version_packet)) {
        AA_LOG_E() << "버전 요청 전송 실패";
        return false;
    }

    auto resp = transport_->Receive();
    if (resp.size() >= 4) {
        std::ostringstream oss;
        for (size_t i = 0; i < (resp.size() < 10 ? resp.size() : 10); ++i) {
            oss << std::hex << std::setw(2) << std::setfill('0') << (int)resp[i] << " ";
        }
        AA_LOG_D() << "Debug: First 10 bytes of resp: " << oss.str();
    }

    if (resp.size() < 10) {
        AA_LOG_E() << "버전 응답 수신 실패 (size: " << resp.size() << ")";
        return false;
    }

    uint16_t major = (resp[6] << 8) | resp[7];
    uint16_t minor = (resp[8] << 8) | resp[9];
    AA_LOG_I() << "버전 응답 수신 완료 (Version: " << major << "." << minor << ")";
    return true;
}

bool Session::DoSslHandshake() {
    // 장치 안정화를 위한 대기
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    crypto_->SetStrategy(std::make_shared<crypto::TlsCryptoStrategy>());

    int handshake_count = 0;
    while (!crypto_->IsHandshakeComplete() && handshake_count++ < 10) {
        AA_LOG_I() << "SSL 핸드셰이크 시도 (" << handshake_count << "/10)...";

        auto out_data = crypto_->GetHandshakeData();
        if (!out_data.empty()) {
            auto packet = aap::Pack(aap::CH_CONTROL, aap::TYPE_SSL_HANDSHAKE, out_data);
            if (!transport_->Send(packet)) {
                AA_LOG_E() << "SSL 핸드셰이크 데이터 전송 실패. 장치 연결이 끊겼을 수 있습니다.";
                break;
            }
        }

        if (crypto_->IsHandshakeComplete()) {
            AA_LOG_I() << "SSL 핸드셰이크 완료!";
            break;
        }

        AA_LOG_I() << "SSL 응답 대기 중...";
        auto in_packet = transport_->Receive();

        if (in_packet.empty()) {
            if (state_.load() == SessionState::DISCONNECTED || !transport_->IsConnected()) {
                AA_LOG_W() << "세션 핸드셰이크 중단됨 (장치 연결 끊어짐 감지됨).";
                break;
            }
            AA_LOG_D() << "SSL 응답 타임아웃 또는 데이터 없음 (재시도)";
            continue;
        }

        if (in_packet.size() >= 6) {
            uint8_t channel = in_packet[0];
            uint16_t msg_type = (in_packet[4] << 8) | in_packet[5];

            if (channel == aap::CH_CONTROL && msg_type == aap::TYPE_SSL_HANDSHAKE) {
                uint16_t aap_len = (in_packet[2] << 8) | in_packet[3];
                if (aap_len >= 2 && in_packet.size() >= static_cast<size_t>(4 + aap_len)) {
                    size_t payload_len = aap_len - 2;
                    std::vector<uint8_t> ssl_payload(in_packet.begin() + 6, in_packet.begin() + 6 + payload_len);
                    crypto_->PutHandshakeData(ssl_payload);
                }
            } else {
                AA_LOG_W() << "SSL 핸드셰이크 중 예기치 않은 패킷 수신 (Ch:" << (int)channel << ", Type:0x" << std::hex << msg_type << ") - 무시함";
                // 재시도 루프에서 다시 Receive 하도록 유도 (handshake_count는 증가시키지 않는 것이 좋으나 일단 놔둠)
            }
        }
    }

    if (!crypto_->IsHandshakeComplete()) {
        AA_LOG_E() << "SSL 핸드셰이크 실패";
        return false;
    }
    return true;
}

bool Session::SendSslAuthComplete() {
    std::vector<uint8_t> auth_complete_payload = {8, 0};  // Status OK
    auto auth_packet = aap::Pack(aap::CH_CONTROL, aap::TYPE_SSL_AUTH_COMPLETE, auth_complete_payload);
    return transport_->Send(auth_packet);
}


void Session::Stop() {
    SessionState expected = state_.load();
    while (expected != SessionState::DISCONNECTED) {
        if (state_.compare_exchange_weak(expected, SessionState::DISCONNECTED)) {
            AA_LOG_I() << "세션이 종료되었습니다 (DISCONNECTED).";

            // 스레드 및 통신 리소스 정리
            if (transport_) {
                transport_->Disconnect();
            }

            // 큐 대기 중인 처리 스레드를 깨움
            queue_cv_.notify_all();

            // 스레드 종료 대기
            if (receive_thread_.joinable()) receive_thread_.join();
            if (process_thread_.joinable()) process_thread_.join();

            break;
        }
    }
}

// 채널이 특정 데이터를 전송하고 싶을 때 호출하는 API
bool Session::SendData(std::shared_ptr<service::IService> service, const std::vector<uint8_t>& payload) {
    if (state_.load() != SessionState::CONNECTED) {
        AA_LOG_E() << "오류: 세션이 연결된 상태가 아닙니다.";
        return false;
    }

    // AAP 헤더 구성 및 암호화
    // Android Auto는 헤더(4바이트)는 평문으로, 그 뒤의 데이터(메시지 타입 + 실제 페이로드)를 암호화합니다.
    uint8_t ch = static_cast<uint8_t>(service->GetType()); // 현재 ServiceType을 채널명으로 매핑 (임시)

    // 서비스에서 준비한 메시지 (보통 [Type(2)] + [Payload])
    std::vector<uint8_t> message_to_encrypt = service->PrepareMessage(payload);
    std::vector<uint8_t> encrypted_payload = crypto_->EncryptData(message_to_encrypt);

    // 전체 패킷: [Header(4)] + [EncryptedPayload]
    uint16_t total_len = static_cast<uint16_t>(encrypted_payload.size());
    std::vector<uint8_t> packet(aap::HEADER_SIZE + encrypted_payload.size());

    packet[0] = ch;
    packet[1] = 0x0b; // Default flags
    packet[2] = (total_len >> 8) & 0xFF;
    packet[3] = total_len & 0xFF;

    std::copy(encrypted_payload.begin(), encrypted_payload.end(), packet.begin() + aap::HEADER_SIZE);

    return transport_->Send(packet);
}

void Session::ReceiveLoop() {
    while (state_.load() != SessionState::DISCONNECTED) {
        // 1. 트랜스포트에서 데이터 수신
        std::vector<uint8_t> data = transport_->Receive();
        if (data.empty()) continue;

        // 2. 메시지를 큐에 삽입 (생산자)
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            message_queue_.push_back(std::move(data));
        }
        queue_cv_.notify_one();
    }
}

void Session::ProcessLoop() {
    while (state_.load() != SessionState::DISCONNECTED) {
        std::vector<uint8_t> packet;

        // 1. 큐에서 메시지 꺼내기
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock,
                           [this]() { return !message_queue_.empty() || state_.load() == SessionState::DISCONNECTED; });

            if (state_.load() == SessionState::DISCONNECTED && message_queue_.empty()) {
                break;
            }

            packet = std::move(message_queue_.front());
            message_queue_.erase(message_queue_.begin());
        }

        if (packet.size() < aap::HEADER_SIZE) continue;

        // 2. 데이터 복호화 (헤더 4바이트 이후부터)
        std::vector<uint8_t> encrypted_part(packet.begin() + aap::HEADER_SIZE, packet.end());
        std::vector<uint8_t> decrypted_part = crypto_->DecryptData(encrypted_part);

        if (decrypted_part.size() < aap::TYPE_SIZE) continue;

        // 3. 메시지 헤더 분석 (채널 및 타입)
        uint8_t channel = packet[0];
        uint16_t msg_type = (decrypted_part[0] << 8) | decrypted_part[1];

        // 4. 서비스 라우팅
        service::ServiceType s_type = static_cast<service::ServiceType>(channel);
        auto service = FindService(s_type);
        if (service) {
            // 타입(2바이트)을 제외한 순수 페이로드 전달
            std::vector<uint8_t> payload(decrypted_part.begin() + aap::TYPE_SIZE, decrypted_part.end());
            service->HandleMessage(payload);
        }
    }
}

// 서비스 타입으로 특정 서비스 인스턴스 검색
std::shared_ptr<service::IService> Session::FindService(service::ServiceType type) {
    std::shared_lock<std::shared_mutex> lock(services_mutex_);
    auto it = services_.find(type);
    if (it != services_.end()) {
        return it->second;
    }
    return nullptr;
}

}  // namespace session
}  // namespace aauto
