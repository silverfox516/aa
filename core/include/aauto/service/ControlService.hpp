#pragma once

#include <atomic>
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
    using PhoneInfoCallback = std::function<void(const session::PhoneInfo&)>;

    ControlService(core::HeadunitConfig config,
                   std::vector<std::shared_ptr<IService>> peer_services);
    ~ControlService() override;

    // Installed by Session so the app layer can be notified once the
    // phone identifies itself in ServiceDiscoveryRequest. Called on the
    // session process thread; the callback must be thread-safe.
    void SetPhoneInfoCallback(PhoneInfoCallback cb) { phone_info_cb_ = std::move(cb); }

    void FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) override {}
    ServiceType GetType() const override { return ServiceType::CONTROL; }
    std::string GetName() const override { return "ControlService"; }

    void OnChannelOpened(uint8_t channel) override;
    void OnSessionStopped() override;

    void SendNavFocusNotification(aap_protobuf::service::control::message::NavFocusType type);

   private:
    void SendServiceDiscoveryResponse();
    void SendPing();
    void HeartbeatLoop();

    core::HeadunitConfig                   config_;
    std::vector<std::shared_ptr<IService>> peer_services_;
    PhoneInfoCallback                      phone_info_cb_;

    std::thread        heartbeat_thread_;
    std::atomic<bool>  heartbeat_running_{false};
};

} // namespace service
} // namespace aauto
