#define LOG_TAG "AA.UsbTransport"
#include "aauto/transport/UsbTransport.hpp"

#include <thread>
#include <chrono>

#include "aauto/utils/Logger.hpp"
#include "aauto/utils/ProtocolUtil.hpp"

namespace aauto {
namespace transport {

UsbTransport::UsbTransport(libusb_device_handle* handle, libusb_context* /*detector_ctx*/)
    : handle_(handle), is_connected_(false), ep_in_(0), ep_out_(0), claimed_interface_(-1) {
    if (handle_) {
        AA_LOG_I() << "Created - handle: " << handle_;
        is_connected_ = true;

        FindEndpoints();

        // Prepare async read transfer
        read_transfer_ = libusb_alloc_transfer(0);
        read_buffer_.resize(65536);
    }
}

UsbTransport::~UsbTransport() {
    Disconnect();
}

void UsbTransport::FindEndpoints() {
    libusb_device* dev = libusb_get_device(handle_);
    libusb_config_descriptor* config;
    if (libusb_get_active_config_descriptor(dev, &config) < 0) return;

    for (int i = 0; i < config->bNumInterfaces; i++) {
        const libusb_interface* inter = &config->interface[i];
        for (int j = 0; j < inter->num_altsetting; j++) {
            const libusb_interface_descriptor* interdesc = &inter->altsetting[j];

            // Skip interfaces that don't have exactly 2 endpoints
            if (interdesc->bNumEndpoints != 2) {
                continue;
            }

            // Use this interface as the target
            claimed_interface_ = interdesc->bInterfaceNumber;

            const libusb_endpoint_descriptor* ep0 = &interdesc->endpoint[0];
            const libusb_endpoint_descriptor* ep1 = &interdesc->endpoint[1];

            // Assign IN/OUT based on endpoint direction bit
            if (ep0->bEndpointAddress & LIBUSB_ENDPOINT_IN) {
                ep_in_ = ep0->bEndpointAddress;
                ep_out_ = ep1->bEndpointAddress;
            } else {
                ep_out_ = ep0->bEndpointAddress;
                ep_in_ = ep1->bEndpointAddress;
            }
            break;
        }
        if (claimed_interface_ != -1) break;
    }
    libusb_free_config_descriptor(config);

    if (claimed_interface_ != -1) {
        AA_LOG_I() << "Interface " << claimed_interface_ << " selected, claiming...";

        if (libusb_kernel_driver_active(handle_, claimed_interface_) == 1) {
            libusb_detach_kernel_driver(handle_, claimed_interface_);
        }

        int rc = -1;
        for (int retry = 0; retry < 3; ++retry) {
            if (is_aborted_.load()) {
                AA_LOG_W() << "Interface claim aborted (device removed)";
                is_connected_ = false;
                return;
            }

            rc = libusb_claim_interface(handle_, claimed_interface_);
            if (rc == 0) break;

            AA_LOG_W() << "Interface " << claimed_interface_ << " claim attempt (" << retry + 1 << "/3): " << libusb_error_name(rc);

            // Check abort flag every 10ms rather than sleeping the full 500ms at once
            for (int wait = 0; wait < 50; ++wait) {
                if (is_aborted_.load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        if (is_aborted_.load()) {
            is_connected_ = false;
            return;
        }

        if (rc != 0) {
            AA_LOG_E() << "Interface " << claimed_interface_ << " claim failed: " << libusb_error_name(rc);
            is_connected_ = false;
            return;
        } else {
            AA_LOG_I() << "Interface " << claimed_interface_ << " claimed";
        }

        char ep_buf[64];
        snprintf(ep_buf, sizeof(ep_buf), "IN: 0x%02x, OUT: 0x%02x", ep_in_, ep_out_);
        AA_LOG_I() << "Endpoints configured - " << ep_buf;

        // Clear halt state on both endpoints
        if (ep_in_ != 0) libusb_clear_halt(handle_, ep_in_);
        if (ep_out_ != 0) libusb_clear_halt(handle_, ep_out_);
    } else {
        AA_LOG_E() << "No interface with exactly 2 endpoints found";
        is_connected_ = false;
    }
}

bool UsbTransport::Connect(const DeviceInfo& device) {
    if (handle_) {
        AA_LOG_I() << "Connect() called - starting read loop";
        is_connected_ = true;
        SubmitReadTransfer();
        send_thread_ = std::thread(&UsbTransport::SendLoop, this);
        return true;
    }
    return false;
}

void UsbTransport::Disconnect() {
    // Guard against multiple calls
    if (is_aborted_.exchange(true)) return;

    is_connected_ = false;

    // Stop send thread first
    send_cv_.notify_all();
    if (send_thread_.joinable()) send_thread_.join();

    // Cancel the read transfer and wait for completion via condition variable.
    // The detector's event_thread processes the cancel callback, so no local event loop is needed.
    if (read_transfer_) {
        libusb_cancel_transfer(read_transfer_);
        std::unique_lock<std::mutex> lock(transfer_mutex_);
        transfer_cv_.wait(lock, [this] { return read_transfer_complete_.load(); });
        libusb_free_transfer(read_transfer_);
        read_transfer_ = nullptr;
    }

    if (handle_) {
        if (claimed_interface_ >= 0) {
            libusb_release_interface(handle_, claimed_interface_);
            claimed_interface_ = -1;
        }
        AA_LOG_D() << "Closing handle: " << handle_;
        libusb_close(handle_);
        handle_ = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        receive_queue_.clear();
    }
    queue_cv_.notify_all();
}

void UsbTransport::SubmitReadTransfer() {
    if (!is_connected_ || !handle_ || !read_transfer_ || ep_in_ == 0) return;

    char ep_buf[16];
    snprintf(ep_buf, sizeof(ep_buf), "0x%02x", ep_in_);
    AA_LOG_D() << "Submitting read transfer (EP: " << ep_buf << ")...";
    libusb_fill_bulk_transfer(read_transfer_, handle_, ep_in_, read_buffer_.data(),
                              read_buffer_.size(), OnReadComplete, this, 0);

    int rc = libusb_submit_transfer(read_transfer_);
    if (rc != 0) {
        AA_LOG_E() << "Read transfer submit failed: " << libusb_error_name(rc);
    }
}

void LIBUSB_CALL UsbTransport::OnReadComplete(struct libusb_transfer* transfer) {
    auto* transport = static_cast<UsbTransport*>(transfer->user_data);
    if (transfer->status != LIBUSB_TRANSFER_COMPLETED && transfer->status != LIBUSB_TRANSFER_CANCELLED) {
        AA_LOG_E() << "OnReadComplete error status: " << transfer->status;
    }
    transport->HandleReadComplete(transfer);
}

void UsbTransport::HandleReadComplete(struct libusb_transfer* transfer) {
    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        if (transfer->actual_length > 0) {
            std::vector<uint8_t> data(transfer->buffer, transfer->buffer + transfer->actual_length);

            AA_LOG_D() << "USB read complete: " << transfer->actual_length << " bytes";
            size_t l = transfer->actual_length > 16 ? 16 : transfer->actual_length;
            AA_LOG_D() << utils::ProtocolUtil::DumpHex(std::vector<uint8_t>(transfer->buffer, transfer->buffer + l), l);

            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                receive_queue_.push_back(std::move(data));
            }
            queue_cv_.notify_one();
        }
    } else if (transfer->status == LIBUSB_TRANSFER_CANCELLED) {
        // Cancellation requested by Disconnect() — signal completion and exit
        read_transfer_complete_.store(true);
        transfer_cv_.notify_one();
        return;
    } else {
        AA_LOG_E() << "Async read failed: " << transfer->status;
        if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE || transfer->status == LIBUSB_TRANSFER_ERROR) {
            AA_LOG_E() << "Fatal read error, forcing disconnect";
            is_connected_ = false;
            read_transfer_complete_.store(true);
            transfer_cv_.notify_one();
            queue_cv_.notify_all();
            return;
        }
    }

    if (!is_aborted_.load()) {
        SubmitReadTransfer();
    } else {
        read_transfer_complete_.store(true);
        transfer_cv_.notify_one();
    }
}


bool UsbTransport::Send(const std::vector<uint8_t>& data) {
    if (!is_connected_ || ep_out_ == 0) return false;
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        send_queue_.push_back(data);
    }
    send_cv_.notify_one();
    return true;
}

void UsbTransport::SendLoop() {
    while (!is_aborted_.load()) {
        std::vector<uint8_t> data;
        {
            std::unique_lock<std::mutex> lock(send_mutex_);
            send_cv_.wait(lock, [this] { return !send_queue_.empty() || is_aborted_.load(); });
            if (is_aborted_.load() && send_queue_.empty()) break;
            data = std::move(send_queue_.front());
            send_queue_.pop_front();
        }
        SendBlocking(data);
    }
}

void UsbTransport::SendBlocking(const std::vector<uint8_t>& data) {
    if (is_aborted_.load() || !handle_ || ep_out_ == 0) return;

    int transferred = 0;
    int rc = libusb_bulk_transfer(handle_, ep_out_,
        const_cast<unsigned char*>(data.data()), static_cast<int>(data.size()),
        &transferred, 1000);

    if (rc == LIBUSB_ERROR_NO_DEVICE || rc == LIBUSB_ERROR_IO) {
        is_connected_ = false;
        queue_cv_.notify_all();
    }
}

std::vector<uint8_t> UsbTransport::Receive() {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    if (receive_queue_.empty()) {
        queue_cv_.wait_for(lock, std::chrono::seconds(2), [this] {
            return !receive_queue_.empty() || !is_connected_;
        });
    }

    if (!receive_queue_.empty()) {
        std::vector<uint8_t> data = std::move(receive_queue_.front());
        receive_queue_.pop_front();
        return data;
    }

    return {};
}

TransportType UsbTransport::GetType() const { return TransportType::USB; }

}  // namespace transport
}  // namespace aauto
