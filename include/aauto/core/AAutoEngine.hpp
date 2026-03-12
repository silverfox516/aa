#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "aauto/core/DeviceManager.hpp"
#include "aauto/core/HeadunitConfig.hpp"
#include "aauto/platform/IPlatform.hpp"
#include "aauto/session/Session.hpp"
#include "aauto/transport/ITransport.hpp"

namespace aauto {
namespace core {

class AAutoEngine {
   public:
    AAutoEngine(DeviceManager& device_manager,
                std::shared_ptr<platform::IPlatform> platform,
                HeadunitConfig config = {});
    ~AAutoEngine();

    bool Initialize();

   private:
    void OnDeviceConnected(const transport::DeviceInfo& device,
                           std::shared_ptr<transport::ITransport> transport);
    void OnDeviceDisconnected(const std::string& device_id);

   private:
    DeviceManager&                      device_manager_;
    std::shared_ptr<platform::IPlatform> platform_;
    HeadunitConfig                       config_;
    ListenerHandle                       listener_handle_;
    std::mutex                           sessions_mutex_;
    std::map<std::string, std::shared_ptr<session::Session>> active_sessions_;
};

} // namespace core
} // namespace aauto
