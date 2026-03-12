#pragma once

#include <memory>

#include "aauto/core/HeadunitConfig.hpp"
#include "aauto/platform/IVideoOutput.hpp"
#include "aauto/service/ServiceBase.hpp"

namespace aauto {
namespace service {

class VideoService : public ServiceBase {
   public:
    VideoService(core::HeadunitConfig config,
                 std::shared_ptr<platform::IVideoOutput> video_output);

    void FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) override;
    void OnChannelOpened(uint8_t channel) override;
    void OnSessionStopped() override;
    ServiceType GetType() const override { return ServiceType::VIDEO; }
    std::string GetName() const override { return "VideoService"; }

   private:
    void HandleSetupRequest(const std::vector<uint8_t>& payload);
    void HandleStartRequest(const std::vector<uint8_t>& payload);
    void SendVideoFocusGain();
    void SendMediaAck();

    core::HeadunitConfig                    config_;
    std::shared_ptr<platform::IVideoOutput> video_output_;
    int32_t                                 session_id_ = 0;
};

} // namespace service
} // namespace aauto
