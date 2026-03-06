#include "aauto/transport/UsbTransport.hpp"

#include <iostream>
#include <thread>
#include <chrono>

namespace aauto {
namespace transport {

UsbTransport::UsbTransport(libusb_device_handle* handle)
    : handle_(handle), is_connected_(false), ep_in_(0), ep_out_(0) {
    if (handle_) {
        printf("[USB] UsbTransport 생성 - Handle: %p\n", (void*)handle_);
        is_connected_ = true;

        // macOS에서는 Configuration 설정이 중요할 수 있음
        libusb_set_configuration(handle_, 1);

        // 인터페이스 0 선점 시도
        if (libusb_kernel_driver_active(handle_, 0) == 1) {
            libusb_detach_kernel_driver(handle_, 0);
        }

        int rc = -1;
        for (int retry = 0; retry < 3; ++retry) {
            rc = libusb_claim_interface(handle_, 0);
            if (rc == 0) break;

            std::cerr << "[USB] 인터페이스 0 클레임 시도 (" << retry + 1 << "/3): " << libusb_error_name(rc) << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        if (rc != 0) {
            std::cerr << "[USB] 인터페이스 0 클레임 최종 실패: " << libusb_error_name(rc) << ". macOS에서는 권한이나 다른 프로세스 점유 문제일 수 있습니다." << std::endl;
        } else {
            printf("[USB] 인터페이스 0 클레임 성공\n");
        }

        FindEndpoints();

        // Async Read 준비
        read_transfer_ = libusb_alloc_transfer(0);
        read_buffer_.resize(16384);
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

    // 엔드포인트 초기화 (Halt 상태 해제)
    if (ep_in_ != 0) libusb_clear_halt(handle_, ep_in_);
    if (ep_out_ != 0) libusb_clear_halt(handle_, ep_out_);
}

bool UsbTransport::Connect(const DeviceInfo& device) {
    if (handle_) {
        printf("[USB] UsbTransport::Connect() 호출됨 - 읽기 루프 시작\n");
        is_connected_ = true;
        SubmitReadTransfer();
        return true;
    }
    return false;
}

void UsbTransport::Disconnect() {
    if (handle_ && is_connected_) {
        is_connected_ = false;

        if (read_transfer_) {
            libusb_cancel_transfer(read_transfer_);
        }

        libusb_release_interface(handle_, 0);
        printf("[USB] UsbTransport 핸들 클로즈 - Handle: %p\n", (void*)handle_);
        libusb_close(handle_);
        handle_ = nullptr;

        if (read_transfer_) {
            // libusb_cancel_transfer 후에 즉시 해제하지 않고 콜백에서 해제하는 것이 안전할 수 있으나,
            // 여기서는 Disconnect 호출 시점에 정리를 완료합니다.
            libusb_free_transfer(read_transfer_);
            read_transfer_ = nullptr;
        }
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        receive_queue_.clear();
    }
    queue_cv_.notify_all();
}

void UsbTransport::SubmitReadTransfer() {
    if (!is_connected_ || !handle_ || !read_transfer_ || ep_in_ == 0) return;

    printf("[USB] Read 전송 제출 중 (EP: 0x%02x)...\n", ep_in_);
    libusb_fill_bulk_transfer(read_transfer_, handle_, ep_in_, read_buffer_.data(),
                              read_buffer_.size(), OnReadComplete, this, 0);

    int rc = libusb_submit_transfer(read_transfer_);
    if (rc != 0) {
        std::cerr << "[USB] Read 전송 제출 실패: " << libusb_error_name(rc) << std::endl;
    }
}

void LIBUSB_CALL UsbTransport::OnReadComplete(struct libusb_transfer* transfer) {
    auto* transport = static_cast<UsbTransport*>(transfer->user_data);
    if (transfer->status != LIBUSB_TRANSFER_COMPLETED && transfer->status != LIBUSB_TRANSFER_CANCELLED) {
        std::cerr << "[USB] OnReadComplete 에러 상태 수신: " << transfer->status << std::endl;
    }
    transport->HandleReadComplete(transfer);
}

static void PrintHexDump(const std::string& prefix, const std::vector<uint8_t>& data) {
    if (data.empty()) return;
    printf("%s (%zu bytes): ", prefix.c_str(), data.size());
    size_t len = data.size() > 32 ? 32 : data.size();
    for (size_t i = 0; i < len; ++i) printf("%02x ", data[i]);
    if (data.size() > 32) printf("...");
    printf("\n");
}

void UsbTransport::HandleReadComplete(struct libusb_transfer* transfer) {
    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        if (transfer->actual_length > 0) {
            std::vector<uint8_t> data(transfer->buffer, transfer->buffer + transfer->actual_length);

            PrintHexDump("[USB] << RECV (Async Handler)", data);

            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                receive_queue_.push_back(std::move(data));
            }
            queue_cv_.notify_one();
        }
    } else if (transfer->status != LIBUSB_TRANSFER_CANCELLED) {
        std::cerr << "[USB] Async Read 실패: " << transfer->status << std::endl;
    }

    if (is_connected_) {
        SubmitReadTransfer();
    }
}


bool UsbTransport::Send(const std::vector<uint8_t>& data) {
    if (!is_connected_ || !handle_ || ep_out_ == 0) return false;

    PrintHexDump("[USB] >> SEND", data);

    struct libusb_transfer* xfer = libusb_alloc_transfer(0);
    if (!xfer) return false;

    struct SendContext {
        bool completed = false;
        int status = 0;
        std::mutex mtx;
        std::condition_variable cv;
    } context;

    libusb_fill_bulk_transfer(xfer, handle_, ep_out_, const_cast<unsigned char*>(data.data()),
                              static_cast<int>(data.size()), [](struct libusb_transfer* t) {
        SendContext* ctx = static_cast<SendContext*>(t->user_data);
        printf("[USB] Send 콜백 호출됨 - Status: %d, Actual: %d\n", t->status, t->actual_length);
        {
            std::lock_guard<std::mutex> lock(ctx->mtx);
            ctx->status = t->status;
            ctx->completed = true;
        }
        ctx->cv.notify_one();
    }, &context, 1000);

    int rc = libusb_submit_transfer(xfer);
    if (rc != 0) {
        std::cerr << "[USB] Send 전송 제출 실패: " << libusb_error_name(rc) << std::endl;
        libusb_free_transfer(xfer);
        return false;
    }

    {
        std::unique_lock<std::mutex> lock(context.mtx);
        if (!context.cv.wait_for(lock, std::chrono::milliseconds(2000), [&context] { return context.completed; })) {
            std::cerr << "[USB] Send 완료 대기 타임아웃 (2.0s)" << std::endl;
        }
    }

    if (context.status != LIBUSB_TRANSFER_COMPLETED) {
        std::cerr << "[USB] Send 최종 실패 - Status: " << context.status << std::endl;
    }

    bool success = (context.status == LIBUSB_TRANSFER_COMPLETED);
    libusb_free_transfer(xfer);
    return success;
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
        PrintHexDump("[USB] << RECV (Async)", data);
        return data;
    }

    return {};
}

TransportType UsbTransport::GetType() const { return TransportType::USB; }

}  // namespace transport
}  // namespace aauto
