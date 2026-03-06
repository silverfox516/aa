#pragma once

#include <memory>
#include <string>
#include <vector>

namespace aauto {
namespace transport {

// 연결 유형 (전략 패턴에 사용될 수 있음)
enum class TransportType { UNKNOWN, USB, WIRELESS };

// 장치 정보를 담는 구조체
struct DeviceInfo {
    std::string id;
    std::string name;
    TransportType type;
};

// 통신 인터페이스 (Strategy Pattern)
// USB나 Wireless 통신의 추상화 계층
class ITransport {
   public:
    virtual ~ITransport() = default;

    virtual bool Connect(const DeviceInfo& device) = 0;
    virtual void Disconnect() = 0;

    // 데이터 송수신
    virtual bool Send(const std::vector<uint8_t>& data) = 0;
    virtual std::vector<uint8_t> Receive() = 0;

    virtual TransportType GetType() const = 0;
};

}  // namespace transport
}  // namespace aauto
