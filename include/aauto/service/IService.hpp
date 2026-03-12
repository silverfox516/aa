#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "aap_protobuf/service/Service.pb.h"

namespace aauto {
namespace service {

enum class ServiceType { CONTROL, AUDIO, VIDEO, INPUT, SENSOR, MIC, BLUETOOTH };

class IService {
   public:
    virtual ~IService() = default;

    virtual void HandleMessage(uint16_t msg_type, const std::vector<uint8_t>& payload) = 0;

    using SendCallback = std::function<bool(uint8_t channel, uint16_t msg_type, const std::vector<uint8_t>&)>;
    virtual void SetSendCallback(SendCallback cb) = 0;

    virtual void FillServiceDefinition(aap_protobuf::service::ServiceConfiguration* service_proto) = 0;
    virtual std::vector<uint8_t> PrepareMessage(const std::vector<uint8_t>& payload) = 0;

    virtual ServiceType GetType() const = 0;
    virtual std::string GetName() const = 0;

    virtual uint8_t GetChannel() const { return channel_; }
    virtual void SetChannel(uint8_t channel) { channel_ = channel; }

    virtual void OnChannelOpened(uint8_t channel) {}
    virtual void OnSessionStopped() {}

   protected:
    uint8_t channel_ = 0;
};

} // namespace service
} // namespace aauto
