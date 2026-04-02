#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "aauto/transport/ITransport.hpp"

namespace aauto {
namespace transport {

/**
 * USB transport backed by an Android file descriptor obtained from
 * UsbDeviceConnection.getFileDescriptor().
 *
 * Architecture mirrors UsbTransport but uses ioctl(USBDEVFS_BULK) for
 * bulk transfers on the USB device fd obtained from UsbDeviceConnection.
 *
 * Threading model:
 *   - read_thread_ : continuous blocking read loop, enqueues received data
 *   - Send()       : synchronous, blocks the caller until the write completes
 *   read_thread_ is started by Connect() and joined by Disconnect().
 */
class AndroidUsbTransport : public ITransport {
public:
    AndroidUsbTransport(int fd, std::string device_id, int ep_in, int ep_out);
    ~AndroidUsbTransport() override;

    // ITransport
    bool Connect(const DeviceInfo& device) override;
    void Disconnect() override;
    bool IsConnected() const override;
    bool Send(const std::vector<uint8_t>& data) override;
    std::vector<uint8_t> Receive() override;
    TransportType GetType() const override;

private:
    void ReadLoop();

    int                  fd_;
    int                  ep_in_;
    int                  ep_out_;
    std::string          device_id_;
    std::atomic<bool>    is_connected_{false};
    std::atomic<bool>    is_aborted_{false};

    std::thread          read_thread_;

    std::mutex           recv_mutex_;
    std::condition_variable recv_cv_;
    std::deque<std::vector<uint8_t>> recv_queue_;

    std::mutex           send_mutex_;  // serialize concurrent senders

    std::vector<uint8_t> read_buf_;

    static constexpr size_t kReadBufSize    = 65536;
    static constexpr int    kMaxSendRetries = 15;  // 15 x 1s before giving up
};

}  // namespace transport
}  // namespace aauto
