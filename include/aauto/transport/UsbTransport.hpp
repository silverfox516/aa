#pragma once

#include <libusb.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <queue>
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
    uint8_t ep_in_;
    uint8_t ep_out_;

    // Async Read Management
    static void LIBUSB_CALL OnReadComplete(struct libusb_transfer* transfer);
    void HandleReadComplete(struct libusb_transfer* transfer);
    void SubmitReadTransfer();

    struct libusb_transfer* read_transfer_ = nullptr;
    std::vector<uint8_t> read_buffer_;

    std::deque<std::vector<uint8_t>> receive_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    void FindEndpoints();
};

}  // namespace transport
}  // namespace aauto
