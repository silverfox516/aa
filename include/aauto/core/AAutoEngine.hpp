#pragma once

#include <map>
#include <memory>
#include <string>

#include "aauto/core/DeviceManager.hpp"
#include "aauto/session/Session.hpp"
#include "aauto/transport/ITransport.hpp"

namespace aauto {
namespace core {

// 프레임워크 초기화와 수명 주기를 담당하는 퍼사드 (Facade Pattern)
class AAutoEngine {
   public:
    AAutoEngine();
    ~AAutoEngine();

    // 시스템(앱) 부팅 시 호출
    bool Initialize();

   private:
    // 연결 이벤트 핸들러
    void OnDeviceConnected(const transport::DeviceInfo& device, std::shared_ptr<transport::ITransport> transport);

    // 연결 해제 이벤트 핸들러
    void OnDeviceDisconnected(const std::string& device_id);

   private:
    // 옵저버 핸들 유지
    core::ListenerHandle listener_handle_;
    // 여러 기기의 다중 연결(Session) 지원
    std::map<std::string, std::shared_ptr<session::Session>> active_sessions_;
};

}  // namespace core
}  // namespace aauto
