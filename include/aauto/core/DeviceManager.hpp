#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>  // Keep for DeviceInfo, if needed elsewhere, but not for listeners_

#include "aauto/transport/ITransport.hpp"

namespace aauto {
namespace core {

// 구독 핸들을 정의 (구독 해지용)
using ListenerHandle = size_t;

// 디바이스 연결 이벤트 관리를 위한 매니저 (Singleton + Observer Pattern 융합)
class DeviceManager {
   public:
    // 연결 시 호출될 콜백 타입 정의
    using ConnectedCallback = std::function<void(const transport::DeviceInfo&, std::shared_ptr<transport::ITransport>)>;
    // 해제 시 호출될 콜백 타입 정의
    using DisconnectedCallback = std::function<void(const std::string&)>;

    struct ListenerRecord {
        ConnectedCallback on_connected;
        DisconnectedCallback on_disconnected;
    };

    static DeviceManager& GetInstance();

    // 옵저버(리스너) 등록. 해지를 위한 고유 핸들(Handle) 반환
    ListenerHandle AddConnectionListener(ConnectedCallback on_connected, DisconnectedCallback on_disconnected);

    // 옵저버(리스너) 해지
    void RemoveListener(ListenerHandle handle);

    // 하위 모듈(예: USB 감지기)에서 호출하여 이벤트 전파
    void NotifyDeviceConnected(const transport::DeviceInfo& device, std::shared_ptr<transport::ITransport> transport);

    // 하위 모듈(예: USB 감지기)에서 호출하여 이벤트 전파
    void NotifyDeviceDisconnected(const std::string& device_id);

   private:
    DeviceManager() = default;
    ~DeviceManager() = default;
    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

   private:
    std::unordered_map<ListenerHandle, ListenerRecord> listeners_;
    std::shared_mutex mutex_;  // 다중 스레드 환경(Read-Write) 안전성 확보
    std::atomic<ListenerHandle> next_handle_{1};
};

}  // namespace core
}  // namespace aauto
