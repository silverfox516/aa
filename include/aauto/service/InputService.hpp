#pragma once

#include <memory>

#include "aauto/core/HeadunitConfig.hpp"
#include "aauto/platform/IVideoOutput.hpp"
#include "aauto/service/ServiceBase.hpp"

namespace aauto {
namespace service {

class InputService : public ServiceBase {
   public:
    InputService(core::HeadunitConfig config,
                 std::shared_ptr<platform::IVideoOutput> video_output);

    void HandleMessage(uint16_t msg_type, const std::vector<uint8_t>& payload) override;
    void FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) override;
    ServiceType GetType() const override { return ServiceType::INPUT; }
    std::string GetName() const override { return "InputService"; }

    void SendTouchEvent(int x, int y, int pointer_id, int action);

   private:
    void HandleBindingRequest(const std::vector<uint8_t>& payload);

    core::HeadunitConfig                    config_;
    std::shared_ptr<platform::IVideoOutput> video_output_;
};

} // namespace service
} // namespace aauto
