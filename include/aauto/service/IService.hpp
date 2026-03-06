#pragma once

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace aauto {
namespace service {

// 서비스 타입 식별
enum class ServiceType { CONTROL, AUDIO, VIDEO, INPUT, SENSOR };

// 개별 서비스의 공통 인터페이스
class IService {
   public:
    virtual ~IService() = default;

    // 메시지 수신 시 처리
    virtual void HandleMessage(const std::vector<uint8_t>& payload) = 0;

    // 메시지 송신 전 포맷팅 (페이로드 받아 패킷 구성)
    virtual std::vector<uint8_t> PrepareMessage(const std::vector<uint8_t>& payload) = 0;

    virtual ServiceType GetType() const = 0;
    virtual std::string GetName() const = 0;
};

// 오디오 서비스 예시
class AudioService : public IService {
   public:
    void HandleMessage(const std::vector<uint8_t>& payload) override {
        std::cout << "[AudioService] 오디오 데이터 처리: " << payload.size() << " bytes\n";
    }

    std::vector<uint8_t> PrepareMessage(const std::vector<uint8_t>& payload) override { return payload; }

    ServiceType GetType() const override { return ServiceType::AUDIO; }
    std::string GetName() const override { return "AudioService"; }
};

// 팩토리 (Factory Method Pattern)
class ServiceFactory {
   public:
    static std::shared_ptr<IService> CreateService(ServiceType type) {
        switch (type) {
            case ServiceType::AUDIO:
                return std::make_shared<AudioService>();
            // 다른 서비스 케이스 생략
            default:
                return nullptr;
        }
    }
};

}  // namespace service
}  // namespace aauto
