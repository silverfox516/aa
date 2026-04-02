#pragma once

#include <memory>
#include <string>
#include <vector>

namespace aauto {
namespace transport {

// Transport type (can be used with the Strategy pattern)
enum class TransportType { UNKNOWN, USB, WIRELESS };

// Holds device connection information
struct DeviceInfo {
    std::string id;
    std::string name;
    TransportType type;
};

// Communication interface (Strategy Pattern)
// Abstraction layer for USB or wireless transports
class ITransport {
   public:
    virtual ~ITransport() = default;

    virtual bool Connect(const DeviceInfo& device) = 0;
    virtual void Disconnect() = 0;
    virtual bool IsConnected() const = 0;

    // Send and receive data
    virtual bool Send(const std::vector<uint8_t>& data) = 0;
    virtual std::vector<uint8_t> Receive() = 0;

    virtual TransportType GetType() const = 0;
};

}  // namespace transport
}  // namespace aauto
