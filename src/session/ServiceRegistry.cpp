#include "aauto/session/ServiceRegistry.hpp"
#include <algorithm>

namespace aauto {
namespace session {

void ServiceRegistry::Register(std::shared_ptr<service::IService> service) {
    if (!service) return;

    std::unique_lock<std::shared_mutex> lock(mutex_);

    uint8_t channel = 0;
    switch (service->GetType()) {
        case service::ServiceType::CONTROL:   channel = 0;   break;
        case service::ServiceType::BLUETOOTH: channel = 255; break;
        default:
            channel = 1;
            while (services_.count(channel) || channel == 255) ++channel;
            break;
    }
    service->SetChannel(channel);
    services_[channel] = std::move(service);
}

std::shared_ptr<service::IService> ServiceRegistry::Find(uint8_t channel) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = services_.find(channel);
    return (it != services_.end()) ? it->second : nullptr;
}

std::vector<std::shared_ptr<service::IService>> ServiceRegistry::All() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<std::shared_ptr<service::IService>> result;
    result.reserve(services_.size());
    for (const auto& [ch, svc] : services_) result.push_back(svc);
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a->GetChannel() < b->GetChannel();
    });
    return result;
}

} // namespace session
} // namespace aauto
