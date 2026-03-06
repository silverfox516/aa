#include "aauto/transport/UsbTransport.hpp"

#include <iostream>

namespace aauto {
namespace transport {

UsbTransport::UsbTransport(libusb_device_handle* handle) : handle_(handle), is_connected_(false) {
    if (handle_) {
        is_connected_ = true;
    }
}

UsbTransport::~UsbTransport() { Disconnect(); }

bool UsbTransport::Connect(const DeviceInfo& device) {
    if (handle_) {
        is_connected_ = true;
        return true;
    }
    return false;
}

void UsbTransport::Disconnect() {
    if (handle_ && is_connected_) {
        libusb_close(handle_);
        handle_ = nullptr;
        is_connected_ = false;
    }
}

bool UsbTransport::Send(const std::vector<uint8_t>& data) {
    if (!is_connected_ || !handle_) return false;

    // 실제로는 Bulk Transfer 등 엔드포인트에 맞춰 전송해야 합니다.
    // int actual_length;
    // int r = libusb_bulk_transfer(handle_, ENDPOINT_OUT, const_cast<unsigned char*>(data.data()), data.size(),
    // &actual_length, 0);

    std::cout << "[USB] 데이터 전송 완료 (" << data.size() << " bytes)" << std::endl;
    return true;
}

std::vector<uint8_t> UsbTransport::Receive() {
    if (!is_connected_ || !handle_) return {};

    // 실제로는 Bulk Transfer 등 엔드포인트에 맞춰 수신해야 합니다.
    std::vector<uint8_t> buffer(1024);
    // int actual_length;
    // int r = libusb_bulk_transfer(handle_, ENDPOINT_IN, buffer.data(), buffer.size(), &actual_length, 0);

    return buffer;
}

TransportType UsbTransport::GetType() const { return TransportType::USB; }

}  // namespace transport
}  // namespace aauto
