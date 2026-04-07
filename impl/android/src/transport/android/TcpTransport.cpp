#define LOG_TAG "AA.IMPL.TcpTransport"

#include "aauto/transport/android/TcpTransport.hpp"

#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "aauto/utils/Logger.hpp"

namespace aauto {
namespace transport {

TcpTransport::TcpTransport(int pre_bound_server_fd, std::string device_id)
    : server_fd_(pre_bound_server_fd), device_id_(std::move(device_id)) {}

TcpTransport::~TcpTransport() {
    Disconnect();
}

bool TcpTransport::Connect(const DeviceInfo& /*device*/) {
    if (server_fd_ < 0) {
        AA_LOG_E() << "Connect: no pre-bound server fd";
        return false;
    }

    AA_LOG_I() << "Waiting for phone TCP connection, id=" << device_id_;

    struct sockaddr_in client_addr{};
    socklen_t len = sizeof(client_addr);
    int client_fd = ::accept(server_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &len);

    // Close the server fd — we only need one connection per session
    ::close(server_fd_);
    server_fd_ = -1;

    if (client_fd < 0) {
        if (!is_aborted_.load()) {
            AA_LOG_E() << "accept() failed: " << strerror(errno);
        }
        return false;
    }

    int flag = 1;
    ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    sock_fd_ = client_fd;
    is_aborted_.store(false);
    is_connected_.store(true);
    read_thread_ = std::thread(&TcpTransport::ReadLoop, this);

    char client_ip[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
    AA_LOG_I() << "Phone connected from " << client_ip << " id=" << device_id_;
    return true;
}

void TcpTransport::Disconnect() {
    if (is_aborted_.exchange(true)) return;

    is_connected_.store(false);

    // Close server_fd_ first to unblock a pending accept()
    if (server_fd_ >= 0) {
        ::close(server_fd_);
        server_fd_ = -1;
    }

    if (sock_fd_ >= 0) {
        ::shutdown(sock_fd_, SHUT_RDWR);
        ::close(sock_fd_);
        sock_fd_ = -1;
    }

    recv_cv_.notify_all();

    if (read_thread_.joinable()) read_thread_.join();

    {
        std::lock_guard<std::mutex> lock(recv_mutex_);
        recv_queue_.clear();
    }

    AA_LOG_I() << "Disconnected: " << device_id_;
}

bool TcpTransport::IsConnected() const {
    return is_connected_.load();
}

bool TcpTransport::Send(const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (!is_connected_.load() || sock_fd_ < 0) return false;

    const uint8_t* ptr = data.data();
    size_t remaining   = data.size();

    while (remaining > 0) {
        if (is_aborted_.load()) return false;

        ssize_t n = ::send(sock_fd_, ptr, remaining, MSG_NOSIGNAL);
        if (n > 0) {
            ptr       += n;
            remaining -= static_cast<size_t>(n);
        } else if (n == 0) {
            AA_LOG_E() << "Send: connection closed";
            is_connected_.store(false);
            recv_cv_.notify_all();
            return false;
        } else {
            if (errno == EINTR) continue;
            AA_LOG_E() << "Send: " << strerror(errno);
            is_connected_.store(false);
            recv_cv_.notify_all();
            return false;
        }
    }
    return true;
}

std::vector<uint8_t> TcpTransport::Receive() {
    std::unique_lock<std::mutex> lock(recv_mutex_);
    recv_cv_.wait_for(lock, std::chrono::seconds(2), [this] {
        return !recv_queue_.empty() || !is_connected_.load();
    });
    if (!recv_queue_.empty()) {
        auto data = std::move(recv_queue_.front());
        recv_queue_.pop_front();
        return data;
    }
    return {};
}

TransportType TcpTransport::GetType() const {
    return TransportType::WIRELESS;
}

void TcpTransport::ReadLoop() {
    std::vector<uint8_t> buf(kReadBufSize);
    AA_LOG_D() << "ReadLoop started";

    while (!is_aborted_.load()) {
        ssize_t n = ::recv(sock_fd_, buf.data(), buf.size(), 0);
        if (n > 0) {
            std::vector<uint8_t> data(buf.begin(), buf.begin() + n);
            {
                std::lock_guard<std::mutex> lock(recv_mutex_);
                recv_queue_.push_back(std::move(data));
            }
            recv_cv_.notify_one();
        } else if (n == 0) {
            if (!is_aborted_.load()) {
                AA_LOG_I() << "ReadLoop: peer closed connection";
                is_connected_.store(false);
                recv_cv_.notify_all();
            }
            break;
        } else {
            if (is_aborted_.load()) break;
            if (errno == EINTR) continue;
            AA_LOG_E() << "ReadLoop: recv failed: " << strerror(errno);
            is_connected_.store(false);
            recv_cv_.notify_all();
            break;
        }
    }
    AA_LOG_D() << "ReadLoop exited";
}

}  // namespace transport
}  // namespace aauto
