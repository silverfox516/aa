#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "aauto/transport/ITransport.hpp"

namespace aauto {
namespace core {

using ListenerHandle = size_t;

class DeviceManager {
   public:
    DeviceManager() = default;
    ~DeviceManager() = default;
    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

    using ConnectedCallback    = std::function<void(const transport::DeviceInfo&, std::shared_ptr<transport::ITransport>)>;
    using DisconnectedCallback = std::function<void(const std::string&)>;

    struct ListenerRecord {
        ConnectedCallback    on_connected;
        DisconnectedCallback on_disconnected;
    };

    ListenerHandle AddConnectionListener(ConnectedCallback on_connected, DisconnectedCallback on_disconnected);
    void RemoveListener(ListenerHandle handle);

    void NotifyDeviceConnected(const transport::DeviceInfo& device, std::shared_ptr<transport::ITransport> transport);
    void NotifyDeviceDisconnected(const std::string& device_id);

   private:
    std::unordered_map<ListenerHandle, ListenerRecord> listeners_;
    std::shared_mutex mutex_;
    std::atomic<ListenerHandle> next_handle_{1};
};

} // namespace core
} // namespace aauto
