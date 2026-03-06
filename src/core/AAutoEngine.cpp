#include "aauto/core/AAutoEngine.hpp"

#include "aauto/crypto/CryptoManager.hpp"
#include "aauto/service/IService.hpp"
#include "aauto/session/SessionBuilder.hpp"

namespace aauto {
namespace core {

AAutoEngine::AAutoEngine() {
    // DeviceManager에 Modern 콜백 방식(std::function)으로 리스너 등록
    listener_handle_ = DeviceManager::GetInstance().AddConnectionListener(
        [this](const transport::DeviceInfo& device, std::shared_ptr<transport::ITransport> transport) {
            this->OnDeviceConnected(device, transport);
        },
        [this](const std::string& device_id) { this->OnDeviceDisconnected(device_id); });
}

AAutoEngine::~AAutoEngine() {
    // 리스너 해제 및 모든 리소스 정리 로직
    DeviceManager::GetInstance().RemoveListener(listener_handle_);

    for (auto& pair : active_sessions_) {
        pair.second->Stop();
    }
}

bool AAutoEngine::Initialize() {
    // TODO: 무선 통신 서버나 USB 핫플러그 감지 스레드 시작
    return true;
}

void AAutoEngine::OnDeviceConnected(const transport::DeviceInfo& device,
                                    std::shared_ptr<transport::ITransport> transport) {
    // 1. 디바이스별 Crypto Strategy 생성
    auto crypto_strategy = std::make_shared<crypto::AesCryptoStrategy>();
    auto crypto_manager = std::make_shared<crypto::CryptoManager>(crypto_strategy);

    // 2. Builder 패턴을 활용한 Session 의존성 주입 조립
    session::SessionBuilder builder;
    auto session = builder.SetTransport(transport)
                       .SetCryptoManager(crypto_manager)
                       .AddService(service::ServiceFactory::CreateService(service::ServiceType::CONTROL))
                       .AddService(service::ServiceFactory::CreateService(service::ServiceType::AUDIO))
                       .AddService(service::ServiceFactory::CreateService(service::ServiceType::VIDEO))
                       .AddService(service::ServiceFactory::CreateService(service::ServiceType::INPUT))
                       .AddService(service::ServiceFactory::CreateService(service::ServiceType::SENSOR))
                       .Build();

    if (!session) return;

    // 3. 세션 시작 및 관리 풀에 추가
    if (session->Start()) {
        active_sessions_[device.id] = session;
    }
}

void AAutoEngine::OnDeviceDisconnected(const std::string& device_id) {
    auto it = active_sessions_.find(device_id);
    if (it != active_sessions_.end()) {
        // 세션 종료 및 정리
        it->second->Stop();
        active_sessions_.erase(it);
    }
}

}  // namespace core
}  // namespace aauto
