#pragma once

#include <atomic>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "aauto/crypto/CryptoManager.hpp"
#include "aauto/service/IService.hpp"
#include "aauto/transport/ITransport.hpp"

namespace aauto {
namespace session {

// 세션의 연결 상태를 정의하는 열거형
enum class SessionState {
    DISCONNECTED,  // 초기 상태 또는 연결 해제됨
    HANDSHAKE,     // 물리적 연결은 되었으나 인증/버전 교환 중
    CONNECTED,     // 정상적으로 통신 가능한 상태
    SUSPENDED      // 일시적인 중단 (예: 백그라운드 전환 등)
};

class Session {
   public:
    Session(std::shared_ptr<transport::ITransport> transport, std::shared_ptr<crypto::CryptoManager> crypto);

    ~Session();

    void RegisterService(std::shared_ptr<service::IService> service);

    bool Start();

    void Stop();

    // 현재 세션의 상태 반환
    SessionState GetState() const { return state_.load(); }

    // 서비스가 특정 데이터를 전송하고 싶을 때 호출하는 API
    bool SendData(std::shared_ptr<service::IService> service, const std::vector<uint8_t>& payload);

   private:
    void ReceiveLoop();
    void ProcessLoop();
    void HeartbeatLoop();

    // 핸드셰이크 세부 단계 (관심사 분리)
    bool DoVersionExchange();
    bool DoSslHandshake();
    bool SendSslAuthComplete();

    // 채용된 채널 ID로 특정 서비스 인스턴스 검색
    std::shared_ptr<service::IService> FindService(uint8_t channel);

   private:
    std::shared_ptr<transport::ITransport> transport_;
    std::shared_ptr<crypto::CryptoManager> crypto_;

    // 채널 ID(uint8_t)를 키로 사용하여 서비스 관리 (동일 타입의 여러 인스턴스 지원)
    std::unordered_map<uint8_t, std::shared_ptr<service::IService>> services_;
    std::shared_mutex services_mutex_;  // services_ 데이터 보호를 위한 Read-Write 뮤텍스

    std::atomic<SessionState> state_{SessionState::DISCONNECTED};

    // Active Object 관리를 위한 요소들
    std::thread receive_thread_;  // I/O 전용 스레드
    std::thread process_thread_;  // 비즈니스 로직 전용 스레드
    std::thread heartbeat_thread_; // 핑 전송용 스레드

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::vector<std::vector<uint8_t>> message_queue_;  // 수신된 암호화 메시지를 담아둘 큐

    // AAP 프래그먼트 재조립용 버퍼 (채널 ID별)
    std::unordered_map<uint8_t, std::vector<uint8_t>> fragment_buffers_;
};

}  // namespace session
}  // namespace aauto
