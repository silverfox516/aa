#pragma once

#include "aauto/service/IService.hpp"
#include <memory>

namespace aauto {
namespace video { class VideoRenderer; }  // forward declare

namespace service {

class VideoService : public IService {
   public:
    void HandleMessage(uint16_t msg_type, const std::vector<uint8_t>& payload) override;
    void SetSendCallback(SendCallback cb) override { send_cb_ = cb; }
    void FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) override;
    void OnChannelOpened(uint8_t channel) override;
    std::vector<uint8_t> PrepareMessage(const std::vector<uint8_t>& payload) override { return payload; }
    ServiceType GetType() const override { return ServiceType::VIDEO; }
    std::string GetName() const override { return "VideoService"; }

    /// VideoRenderer를 주입 - 비디오 프레임 수신 시 여기로 전달됩니다.
    void SetRenderer(std::shared_ptr<aauto::video::VideoRenderer> renderer) {
        renderer_ = std::move(renderer);
    }

   private:
    void HandleSetupRequest(const std::vector<uint8_t>& payload);
    void HandleStartRequest(const std::vector<uint8_t>& payload);
    void SendVideoFocusGain();
    void SendMediaAck();

    SendCallback send_cb_;
    int32_t session_id_ = 0;
    std::shared_ptr<aauto::video::VideoRenderer> renderer_;
};

}  // namespace service
}  // namespace aauto
