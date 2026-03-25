#pragma once

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "aauto/service/IService.hpp"

namespace aauto {
namespace session {

// Owns the channel→service mapping for one session.
// Assigns channel numbers and provides thread-safe lookup.
class ServiceRegistry {
   public:
    // Register a service; assigns its channel number automatically.
    void Register(std::shared_ptr<service::IService> service);

    // Returns null if not found.
    std::shared_ptr<service::IService> Find(uint8_t channel) const;

    // Returns all services in ascending channel order (for discovery response).
    std::vector<std::shared_ptr<service::IService>> All() const;

   private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<uint8_t, std::shared_ptr<service::IService>> services_;
};

} // namespace session
} // namespace aauto
