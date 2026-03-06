#include "aauto/transport/UsbTransport.hpp"

#include <iostream>

namespace aauto {
namespace transport {

UsbTransport::UsbTransport(libusb_device_handle* handle)
    : handle_(handle), is_connected_(false), ep_in_(0), ep_out_(0) {
    if (handle_) {
        is_connected_ = true;
        // 인터페이스 0 선점 시도 전, 커널 드라이버가 붙어있다면 분리 (특히 macOS/Linux 대응)
        if (libusb_kernel_driver_active(handle_, 0) == 1) {
            libusb_detach_kernel_driver(handle_, 0);
        }
        int rc = libusb_claim_interface(handle_, 0);
        if (rc != 0) {
            std::cerr << "[USB] 인터페이스 0 클레임 실패: " << libusb_error_name(rc) << std::endl;
        }
        FindEndpoints();
    }
}

UsbTransport::~UsbTransport() { Disconnect(); }

void UsbTransport::FindEndpoints() {
    libusb_device* dev = libusb_get_device(handle_);
    libusb_config_descriptor* config;
    if (libusb_get_active_config_descriptor(dev, &config) < 0) return;

    for (int i = 0; i < config->bNumInterfaces; i++) {
        const libusb_interface* inter = &config->interface[i];
        for (int j = 0; j < inter->num_altsetting; j++) {
            const libusb_interface_descriptor* interdesc = &inter->altsetting[j];
            for (int k = 0; k < interdesc->bNumEndpoints; k++) {
                const libusb_endpoint_descriptor* epdesc = &interdesc->endpoint[k];
                bool is_bulk = (epdesc->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK;
                if (is_bulk) {
                    if (epdesc->bEndpointAddress & LIBUSB_ENDPOINT_IN) {
                        ep_in_ = epdesc->bEndpointAddress;
                    } else {
                        ep_out_ = epdesc->bEndpointAddress;
                    }
                }
            }
        }
    }
    libusb_free_config_descriptor(config);
    printf("[USB] 엔드포인트 발견 - IN: 0x%02x, OUT: 0x%02x\n", ep_in_, ep_out_);
}

bool UsbTransport::Connect(const DeviceInfo& device) {
    if (handle_) {
        is_connected_ = true;
        return true;
    }
    return false;
}

void UsbTransport::Disconnect() {
    if (handle_ && is_connected_) {
        libusb_release_interface(handle_, 0);
        libusb_close(handle_);
        handle_ = nullptr;
        is_connected_ = false;
    }
}

static void PrintHexDump(const std::string& prefix, const std::vector<uint8_t>& data) {
    if (data.empty()) return;
    printf("%s (%zu bytes): ", prefix.c_str(), data.size());
    size_t len = data.size() > 32 ? 32 : data.size();
    for (size_t i = 0; i < len; ++i) printf("%02x ", data[i]);
    if (data.size() > 32) printf("...");
    printf("\n");
}

bool UsbTransport::Send(const std::vector<uint8_t>& data) {
    if (!is_connected_ || !handle_ || ep_out_ == 0) return false;

    PrintHexDump("[USB] >> SEND", data);

    int actual_length;
    int rc = libusb_bulk_transfer(handle_, ep_out_, const_cast<unsigned char*>(data.data()),
                                  static_cast<int>(data.size()), &actual_length, 2000);

    if (rc != 0) {
        std::cerr << "[USB] 벌크 전송 실패: " << libusb_error_name(rc) << std::endl;
        return false;
    }
    return true;
}

std::vector<uint8_t> UsbTransport::Receive() {
    if (!is_connected_ || !handle_ || ep_in_ == 0) return {};

    std::vector<uint8_t> buffer(16384);
    int actual_length = 0;
    int rc = libusb_bulk_transfer(handle_, ep_in_, buffer.data(), buffer.size(), &actual_length, 2000);

    if (rc == LIBUSB_ERROR_TIMEOUT) {
        return {};
    }

    if (rc != 0) {
        std::cerr << "[USB] 벌크 수신 실패: " << libusb_error_name(rc) << std::endl;
        return {};
    }

    if (actual_length > 0) {
        buffer.resize(actual_length);
        PrintHexDump("[USB] << RECV", buffer);
        return buffer;
    }
    return {};
}

TransportType UsbTransport::GetType() const { return TransportType::USB; }

}  // namespace transport
}  // namespace aauto
