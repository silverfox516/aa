#pragma once

#include "aauto/service/IService.hpp"
#include "aauto/video/VideoRenderer.hpp"
#include <memory>

namespace aauto {
namespace service {

class InputService : public IService {
   public:
    void HandleMessage(uint16_t msg_type, const std::vector<uint8_t>& payload) override;
    void SetSendCallback(SendCallback cb) override { send_cb_ = cb; }
    void FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) override;
    std::vector<uint8_t> PrepareMessage(const std::vector<uint8_t>& payload) override { return payload; }
    ServiceType GetType() const override { return ServiceType::INPUT; }
    std::string GetName() const override { return "InputService"; }

    /// VideoRenderer의 터치 콜백을 이 InputService에 연결합니다.
    void AttachToRenderer(std::shared_ptr<aauto::video::VideoRenderer> renderer);

    /// 터치 이벤트를 폰으로 전송 (action: 0=DOWN, 1=UP, 2=MOVED)
    void SendTouchEvent(int x, int y, int pointer_id, int action);

   private:
    void HandleBindingRequest(const std::vector<uint8_t>& payload);

    SendCallback send_cb_;
};

}  // namespace service
}  // namespace aauto
