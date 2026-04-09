#pragma once

#include <cstdint>
#include <vector>

#include "aauto/service/ServiceBase.hpp"
#include "aap_protobuf/service/inputsource/message/TouchScreenType.pb.h"

namespace aauto {
namespace service {

// Options the app layer must supply to enable the input source service.
// `supported_keycodes` are AAP keycode values (see InputReport.proto);
// the app must translate any platform-specific key event values before
// sending them via SendTouchEvent / SendKeyEvent.
struct InputServiceConfig {
    uint32_t touch_width;
    uint32_t touch_height;
    aap_protobuf::service::inputsource::message::TouchScreenType touch_type
        = aap_protobuf::service::inputsource::message::CAPACITIVE;
    bool                  is_secondary = false;
    std::vector<int32_t>  supported_keycodes;
};

// Sends touch events from the head unit to the phone. The app layer drives
// SendTouchEvent directly with coordinates from its surface; the service has
// no callback registration with any platform output object.
class InputService : public ServiceBase {
   public:
    explicit InputService(InputServiceConfig config);

    void FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) override;
    ServiceType GetType() const override { return ServiceType::INPUT; }
    std::string GetName() const override { return "InputService"; }

    void SendTouchEvent(int x, int y, int pointer_id, int action);

   private:
    void HandleBindingRequest(const std::vector<uint8_t>& payload);

    InputServiceConfig config_;
};

} // namespace service
} // namespace aauto
