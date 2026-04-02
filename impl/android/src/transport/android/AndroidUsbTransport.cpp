#define LOG_TAG "AA.AndroidUsbTransport"

#include "aauto/transport/android/AndroidUsbTransport.hpp"

#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>

#include "aauto/utils/Logger.hpp"

namespace aauto {
namespace transport {

AndroidUsbTransport::AndroidUsbTransport(int fd, std::string device_id, int ep_in, int ep_out)
    : fd_(fd)
    , ep_in_(ep_in)
    , ep_out_(ep_out)
    , device_id_(std::move(device_id)) {
    read_buf_.resize(kReadBufSize);
    AA_LOG_I() << "Created with fd=" << fd_ << " ep_in=0x" << std::hex << ep_in_
               << " ep_out=0x" << ep_out_ << std::dec << " id=" << device_id_;
}

AndroidUsbTransport::~AndroidUsbTransport() {
    Disconnect();
}

bool AndroidUsbTransport::Connect(const DeviceInfo& /*device*/) {
    if (fd_ < 0) {
        AA_LOG_E() << "Connect called with invalid fd";
        return false;
    }
    is_aborted_.store(false);
    is_connected_.store(true);
    read_thread_ = std::thread(&AndroidUsbTransport::ReadLoop, this);
    AA_LOG_I() << "Connected, I/O threads started";
    return true;
}

void AndroidUsbTransport::Disconnect() {
    if (is_aborted_.exchange(true)) return;

    is_connected_.store(false);

    // Close fd to unblock ioctl in ReadLoop and any blocked Send()
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }

    recv_cv_.notify_all();

    if (read_thread_.joinable()) read_thread_.join();

    {
        std::lock_guard<std::mutex> lock(recv_mutex_);
        recv_queue_.clear();
    }

    AA_LOG_I() << "Disconnected";
}

bool AndroidUsbTransport::IsConnected() const {
    return is_connected_.load();
}

bool AndroidUsbTransport::Send(const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(send_mutex_);

    if (!is_connected_.load() || fd_ < 0) return false;

    const uint8_t* ptr = data.data();
    size_t remaining   = data.size();
    int retries        = 0;

    while (remaining > 0) {
        if (is_aborted_.load()) return false;

        struct usbdevfs_bulktransfer bulk = {};
        bulk.ep      = static_cast<unsigned int>(ep_out_);
        bulk.len     = static_cast<unsigned int>(remaining);
        bulk.timeout = 1000;  // ms
        bulk.data    = const_cast<uint8_t*>(ptr);

        int n = ::ioctl(fd_, USBDEVFS_BULK, &bulk);
        if (n > 0) {
            ptr      += n;
            remaining -= static_cast<size_t>(n);
            retries   = 0;
        } else if (n < 0 && errno == ETIMEDOUT && retries < kMaxSendRetries) {
            ++retries;
            AA_LOG_W() << "Send: write timeout, retry " << retries << "/" << kMaxSendRetries;
        } else {
            AA_LOG_E() << "Send: ioctl error: " << strerror(errno);
            is_connected_.store(false);
            recv_cv_.notify_all();
            return false;
        }
    }
    return true;
}

std::vector<uint8_t> AndroidUsbTransport::Receive() {
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

TransportType AndroidUsbTransport::GetType() const {
    return TransportType::USB;
}

// ─── Private ──────────────────────────────────────────────────────────────────

void AndroidUsbTransport::ReadLoop() {
    AA_LOG_D() << "ReadLoop started";
    while (!is_aborted_.load()) {
        struct usbdevfs_bulktransfer bulk = {};
        bulk.ep      = static_cast<unsigned int>(ep_in_);
        bulk.len     = static_cast<unsigned int>(read_buf_.size());
        bulk.timeout = 500;  // ms — short so Disconnect() is responsive
        bulk.data    = read_buf_.data();

        int n = ::ioctl(fd_, USBDEVFS_BULK, &bulk);
        if (n > 0) {
            std::vector<uint8_t> data(read_buf_.begin(), read_buf_.begin() + n);
            {
                std::lock_guard<std::mutex> lock(recv_mutex_);
                recv_queue_.push_back(std::move(data));
            }
            recv_cv_.notify_one();
        } else if (n < 0 && errno == ETIMEDOUT) {
            continue;  // Normal timeout, retry
        } else {
            if (is_aborted_.load()) break;  // Expected from Disconnect()
            AA_LOG_E() << "ReadLoop: ioctl error: " << strerror(errno);
            is_connected_.store(false);
            recv_cv_.notify_all();
            break;
        }
    }
    AA_LOG_D() << "ReadLoop exited";
}

}  // namespace transport
}  // namespace aauto
