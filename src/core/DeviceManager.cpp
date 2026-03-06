#include "aauto/core/DeviceManager.hpp"

#include <mutex>

namespace aauto {
namespace core {

DeviceManager& DeviceManager::GetInstance() {
    static DeviceManager instance;
    return instance;
}

ListenerHandle DeviceManager::AddConnectionListener(ConnectedCallback on_connected,
                                                    DisconnectedCallback on_disconnected) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    ListenerHandle handle = next_handle_++;
    listeners_[handle] = {std::move(on_connected), std::move(on_disconnected)};
    return handle;
}

void DeviceManager::RemoveListener(ListenerHandle handle) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    listeners_.erase(handle);
}

void DeviceManager::NotifyDeviceConnected(const transport::DeviceInfo& device,
                                          std::shared_ptr<transport::ITransport> transport) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (const auto& pair : listeners_) {
        if (pair.second.on_connected) {
            pair.second.on_connected(device, transport);
        }
    }
}

void DeviceManager::NotifyDeviceDisconnected(const std::string& device_id) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (const auto& pair : listeners_) {
        if (pair.second.on_disconnected) {
            pair.second.on_disconnected(device_id);
        }
    }
}

}  // namespace core
}  // namespace aauto
