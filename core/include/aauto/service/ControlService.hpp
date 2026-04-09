#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "aauto/core/HeadunitConfig.hpp"
#include "aauto/service/ServiceBase.hpp"
#include "aauto/session/PhoneInfo.hpp"
#include "aap_protobuf/service/control/message/NavFocusType.pb.h"

namespace aauto {
namespace service {

class ControlService : public ServiceBase {
   public:
    using PhoneInfoCallback   = std::function<void(const session::PhoneInfo&)>;
    using SessionCloseCallback = std::function<void()>;

    ControlService(core::HeadunitConfig config,
                   std::vector<std::shared_ptr<IService>> peer_services);
    ~ControlService() override;

    // Installed by Session so the app layer can be notified once the
    // phone identifies itself in ServiceDiscoveryRequest. Called on the
    // session process thread; the callback must be thread-safe.
    void SetPhoneInfoCallback(PhoneInfoCallback cb) { phone_info_cb_ = std::move(cb); }

    // Installed by Session::Start. Fired on either of:
    //   - PING_RESPONSE timeout (kPingTimeout)
    //   - phone-initiated BYEBYE_REQUEST
    // The callback must trigger Session::Stop and must be safe to call
    // from a worker thread.
    void SetSessionCloseCallback(SessionCloseCallback cb) { session_close_cb_ = std::move(cb); }

    void FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) override {}
    ServiceType GetType() const override { return ServiceType::CONTROL; }
    std::string GetName() const override { return "ControlService"; }

    void OnChannelOpened(uint8_t channel) override;
    void OnSessionStopped() override;

    void SendNavFocusNotification(aap_protobuf::service::control::message::NavFocusType type);

   private:
    // Ping cadence. The reference openauto SDR ping_configuration is
    // interval_ms=1000, but on this transport that 5x increase in send
    // pressure correlates with USB write back-pressure deadlocks; use a
    // less aggressive 5 s interval until the underlying buffering issue
    // is resolved. Timeout is intentionally tighter than 3x interval so
    // a stalled phone is detected before transport-layer write timeouts.
    static constexpr auto kPingInterval = std::chrono::seconds(5);
    static constexpr auto kPingTimeout  = std::chrono::seconds(10);

    void SendServiceDiscoveryResponse();
    void SendPing();
    void HeartbeatLoop();
    void TriggerSessionClose(const char* reason);

    core::HeadunitConfig                   config_;
    std::vector<std::shared_ptr<IService>> peer_services_;
    PhoneInfoCallback                      phone_info_cb_;
    SessionCloseCallback                   session_close_cb_;

    std::thread        heartbeat_thread_;
    std::atomic<bool>  heartbeat_running_{false};

    // steady_clock since-epoch nanoseconds. Updated on every PING_RESPONSE.
    std::atomic<int64_t> last_pong_ns_{0};
    std::atomic<bool>    close_triggered_{false};
};

} // namespace service
} // namespace aauto
