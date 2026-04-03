#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "aauto/core/HeadunitConfig.hpp"
#include "aauto/service/ServiceBase.hpp"

namespace aauto {
namespace service {

class ControlService : public ServiceBase {
   public:
    ControlService(core::HeadunitConfig config,
                   std::vector<std::shared_ptr<IService>> peer_services);
    ~ControlService() override;

    void FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) override {}
    ServiceType GetType() const override { return ServiceType::CONTROL; }
    std::string GetName() const override { return "ControlService"; }

    void OnChannelOpened(uint8_t channel) override;
    void OnSessionStopped() override;

    void SendAudioFocusNotification(int state);

    /** Grant audio focus: responds GAIN to future phone requests and notifies immediately. */
    void SendAudioFocusGain();

    /** Revoke audio focus: responds LOSS to future phone requests and notifies immediately. */
    void SendAudioFocusLoss();
    void SendNavFocusNotification(int type);

   private:
    void SendServiceDiscoveryResponse();
    void SendPing();
    void HeartbeatLoop();

    core::HeadunitConfig                   config_;
    std::vector<std::shared_ptr<IService>> peer_services_;

    std::thread        heartbeat_thread_;
    std::atomic<bool>  heartbeat_running_{false};
    bool               audio_focus_granted_{false};
};

} // namespace service
} // namespace aauto
