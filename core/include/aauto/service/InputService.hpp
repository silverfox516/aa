#pragma once

#include "aauto/core/HeadunitConfig.hpp"
#include "aauto/service/ServiceBase.hpp"

namespace aauto {
namespace service {

// Sends touch events from the head unit to the phone. The app layer drives
// SendTouchEvent directly with coordinates from its surface; the service has
// no callback registration with any platform output object.
class InputService : public ServiceBase {
   public:
    explicit InputService(core::HeadunitConfig config);

    void FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) override;
    ServiceType GetType() const override { return ServiceType::INPUT; }
    std::string GetName() const override { return "InputService"; }

    void SendTouchEvent(int x, int y, int pointer_id, int action);

   private:
    void HandleBindingRequest(const std::vector<uint8_t>& payload);

    core::HeadunitConfig config_;
};

} // namespace service
} // namespace aauto
