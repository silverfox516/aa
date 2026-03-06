#pragma once

#include <libusb.h>

#include <string>
#include <vector>

#include "aauto/transport/ITransport.hpp"

namespace aauto {
namespace transport {

class UsbTransport : public ITransport {
   public:
    explicit UsbTransport(libusb_device_handle* handle);
    ~UsbTransport() override;

    bool Connect(const DeviceInfo& device) override;
    void Disconnect() override;
    bool Send(const std::vector<uint8_t>& data) override;
    std::vector<uint8_t> Receive() override;
    TransportType GetType() const override;

   private:
    libusb_device_handle* handle_;
    bool is_connected_;
};

}  // namespace transport
}  // namespace aauto
