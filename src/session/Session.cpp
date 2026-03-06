#include "aauto/session/Session.hpp"

namespace aauto {
namespace session {

Session::Session(std::shared_ptr<transport::ITransport> transport, std::shared_ptr<crypto::CryptoManager> crypto)
    : transport_(std::move(transport)), crypto_(std::move(crypto)), state_(SessionState::DISCONNECTED) {}

Session::~Session() { Stop(); }

void Session::RegisterService(std::shared_ptr<service::IService> service) {
    if (!service) return;

    std::unique_lock<std::shared_mutex> lock(services_mutex_);
    services_[service->GetType()] = service;
    std::cout << "[Session] 서비스 등록됨: " << service->GetName() << std::endl;
}

bool Session::Start() {
    if (!transport_ || !crypto_) return false;

    // 상태 전환: DISCONNECTED -> HANDSHAKE
    SessionState expected = SessionState::DISCONNECTED;
    if (!state_.compare_exchange_strong(expected, SessionState::HANDSHAKE)) {
        return false;  // 이미 시작 중이거나 실행 중
    }

    std::cout << "[Session] 핸드셰이크를 시작합니다..." << std::endl;
    // TODO: Handshake 통신 로직 진행 후 성공 시 CONNECTED로 변경
    // 우선 임시로 바로 CONNECTED 상태로 전환
    state_.store(SessionState::CONNECTED);
    std::cout << "[Session] 세션이 정상적으로 연결되었습니다 (CONNECTED)." << std::endl;

    // 수신 스레드와 처리 스레드 시작
    receive_thread_ = std::thread(&Session::ReceiveLoop, this);
    process_thread_ = std::thread(&Session::ProcessLoop, this);

    return true;
}

void Session::Stop() {
    SessionState expected = state_.load();
    while (expected != SessionState::DISCONNECTED) {
        if (state_.compare_exchange_weak(expected, SessionState::DISCONNECTED)) {
            std::cout << "[Session] 세션이 종료되었습니다 (DISCONNECTED)." << std::endl;

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
    // 연결된 상태에서만 데이터 전송 허용 (State 방어 코드)
    if (state_.load() != SessionState::CONNECTED) {
        std::cerr << "[Session] 오류: 세션이 연결된 상태가 아닙니다. 전송 차단!" << std::endl;
        return false;
    }

    // 1. 서비스가 메세지 포맷에 맞게 패킷 구성
    std::vector<uint8_t> message = service->PrepareMessage(payload);

    // 2. 메시지 암호화
    std::vector<uint8_t> encrypted = crypto_->EncryptData(message);

    // 3. 트랜스포트 계층을 통해 전송 (뮤텍스 적용 필요할 수 있음)
    return transport_->Send(encrypted);
}

void Session::ReceiveLoop() {
    while (state_.load() != SessionState::DISCONNECTED) {
        // 1. 트랜스포트에서 데이터 수신
        std::vector<uint8_t> encrypted_data = transport_->Receive();
        if (encrypted_data.empty()) continue;

        // 2. 메시지를 큐에 삽입 (생산자)
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            message_queue_.push_back(std::move(encrypted_data));
        }
        queue_cv_.notify_one();
    }
}

void Session::ProcessLoop() {
    while (state_.load() != SessionState::DISCONNECTED) {
        std::vector<uint8_t> encrypted_data;

        // 1. 큐에서 메시지 꺼내기 (소비자)
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock,
                           [this]() { return !message_queue_.empty() || state_.load() == SessionState::DISCONNECTED; });

            if (state_.load() == SessionState::DISCONNECTED && message_queue_.empty()) {
                break;
            }

            encrypted_data = std::move(message_queue_.front());
            message_queue_.erase(message_queue_.begin());
        }

        // 2. 데이터 복호화
        std::vector<uint8_t> decrypted_data = crypto_->DecryptData(encrypted_data);

        // 3. 메시지 헤더 분석하여 어떤 서비스(채널) 목적지인지 식별
        service::ServiceType type = ParseServiceType(decrypted_data);
        std::vector<uint8_t> payload = GetPayload(decrypted_data);

        // 4. 해당 서비스로 데이터 전달 (라우팅)
        auto service = FindService(type);
        if (service) {
            service->HandleMessage(payload);
        }
    }
}

// 임시 파서
service::ServiceType Session::ParseServiceType(const std::vector<uint8_t>& /*data*/) {
    return service::ServiceType::CONTROL;  // 기본값
}

// 임시 페이로드 추출기
std::vector<uint8_t> Session::GetPayload(const std::vector<uint8_t>& data) { return data; }

// 서비스 탐색 헬퍼 함수
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
