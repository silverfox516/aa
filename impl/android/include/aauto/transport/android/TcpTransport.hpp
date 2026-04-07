#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "aauto/transport/ITransport.hpp"

namespace aauto {
namespace transport {

/**
 * TCP transport for Wireless Android Auto (AAW).
 *
 * HU acts as TCP server. The server socket must already be bound and listening
 * before this object is constructed (caller uses android.system.Os to
 * socket/bind/listen, then passes the fd here).
 *
 * Connect() performs only accept(), which blocks until the phone connects after
 * joining the HU hotspot.  This separation ensures port 5277 is open before
 * the RFCOMM handshake sends START_RESPONSE.
 */
class TcpTransport : public ITransport {
public:
    /**
     * @param pre_bound_server_fd  Already-bound, listening TCP server socket.
     *                             TcpTransport takes ownership (will close it).
     * @param device_id            Identifies the phone device (BT MAC address).
     */
    TcpTransport(int pre_bound_server_fd, std::string device_id);
    ~TcpTransport() override;

    // ITransport
    bool Connect(const DeviceInfo& device) override;
    void Disconnect() override;
    bool IsConnected() const override;
    bool Send(const std::vector<uint8_t>& data) override;
    std::vector<uint8_t> Receive() override;
    TransportType GetType() const override;

private:
    void ReadLoop();

    int                   server_fd_;        // pre-bound listening socket
    int                   sock_fd_ = -1;     // accepted client socket
    std::string           device_id_;
    std::atomic<bool>     is_connected_{false};
    std::atomic<bool>     is_aborted_{false};

    std::thread           read_thread_;

    std::mutex            recv_mutex_;
    std::condition_variable recv_cv_;
    std::deque<std::vector<uint8_t>> recv_queue_;

    std::mutex            send_mutex_;

    static constexpr size_t kReadBufSize = 65536;
};

}  // namespace transport
}  // namespace aauto
