#define LOG_TAG "UsbTransport"
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
        AA_LOG_I() << "생성 - Handle: " << handle_;
        is_connected_ = true;

        FindEndpoints();

        // Async Read 준비
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
            
            // 엔드포인트 숫자가 2개가 아니면 패스
            if (interdesc->bNumEndpoints != 2) {
                continue;
            }

            // 2개이면 이 인터페이스를 타겟으로 잡음
            claimed_interface_ = interdesc->bInterfaceNumber;

            const libusb_endpoint_descriptor* ep0 = &interdesc->endpoint[0];
            const libusb_endpoint_descriptor* ep1 = &interdesc->endpoint[1];

            // 0번이 in 이면 1은 out, 혹은 그 반대로 설정
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
        AA_LOG_I() << "인터페이스 " << claimed_interface_ << " 선택됨. 클레임 시도...";

        if (libusb_kernel_driver_active(handle_, claimed_interface_) == 1) {
            libusb_detach_kernel_driver(handle_, claimed_interface_);
        }

        int rc = -1;
        for (int retry = 0; retry < 3; ++retry) {
            if (is_aborted_.load()) {
                AA_LOG_W() << "인터페이스 클레임 중단됨 (장치 해제됨)";
                is_connected_ = false;
                return;
            }

            rc = libusb_claim_interface(handle_, claimed_interface_);
            if (rc == 0) break;
            
            AA_LOG_W() << "인터페이스 " << claimed_interface_ << " 클레임 시도 (" << retry + 1 << "/3): " << libusb_error_name(rc);
            
            // 500ms 를 통으로 sleep 하지 않고, 중간중간 중단 여부를 10ms 단위로 빠르게 체크
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
            AA_LOG_E() << "인터페이스 " << claimed_interface_ << " 클레임 최종 실패: " << libusb_error_name(rc);
            is_connected_ = false;
            return;
        } else {
            AA_LOG_I() << "인터페이스 " << claimed_interface_ << " 클레임 성공";
        }

        char ep_buf[64];
        snprintf(ep_buf, sizeof(ep_buf), "IN: 0x%02x, OUT: 0x%02x", ep_in_, ep_out_);
        AA_LOG_I() << "엔드포인트 설정 완료 - " << ep_buf;

        // 엔드포인트 초기화 (Halt 상태 해제)
        if (ep_in_ != 0) libusb_clear_halt(handle_, ep_in_);
        if (ep_out_ != 0) libusb_clear_halt(handle_, ep_out_);
    } else {
        AA_LOG_E() << "조건에 맞는 엔드포인트(2개)를 가진 인터페이스를 찾지 못했습니다.";
        is_connected_ = false;
    }
}

bool UsbTransport::Connect(const DeviceInfo& device) {
    if (handle_) {
        AA_LOG_I() << "Connect() 호출됨 - 읽기 루프 시작";
        is_connected_ = true;
        SubmitReadTransfer();
        send_thread_ = std::thread(&UsbTransport::SendLoop, this);
        return true;
    }
    return false;
}

void UsbTransport::Disconnect() {
    // 중복 호출 방지
    if (is_aborted_.exchange(true)) return;

    is_connected_ = false;

    // send 스레드 먼저 종료
    send_cv_.notify_all();
    if (send_thread_.joinable()) send_thread_.join();

    // read transfer 취소 — detector의 event_thread가 cancel 콜백을 처리하므로
    // condition variable로 완료를 기다림 (자체 event loop 불필요)
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
        AA_LOG_D() << "핸들 클로즈 - Handle: " << handle_;
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
    AA_LOG_D() << "수신 요청 제출 중 (EP: " << ep_buf << ")...";
    libusb_fill_bulk_transfer(read_transfer_, handle_, ep_in_, read_buffer_.data(),
                              read_buffer_.size(), OnReadComplete, this, 0);

    int rc = libusb_submit_transfer(read_transfer_);
    if (rc != 0) {
        AA_LOG_E() << "수신 요청 제출 실패: " << libusb_error_name(rc);
    }
}

void LIBUSB_CALL UsbTransport::OnReadComplete(struct libusb_transfer* transfer) {
    auto* transport = static_cast<UsbTransport*>(transfer->user_data);
    if (transfer->status != LIBUSB_TRANSFER_COMPLETED && transfer->status != LIBUSB_TRANSFER_CANCELLED) {
        AA_LOG_E() << "OnReadComplete 에러 상태 수신: " << transfer->status;
    }
    transport->HandleReadComplete(transfer);
}

void UsbTransport::HandleReadComplete(struct libusb_transfer* transfer) {
    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        if (transfer->actual_length > 0) {
            std::vector<uint8_t> data(transfer->buffer, transfer->buffer + transfer->actual_length);

            AA_LOG_D() << "USB 수신 완료: " << transfer->actual_length << " bytes";
            size_t l = transfer->actual_length > 16 ? 16 : transfer->actual_length;
            AA_LOG_D() << utils::ProtocolUtil::DumpHex(std::vector<uint8_t>(transfer->buffer, transfer->buffer + l), l);

            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                receive_queue_.push_back(std::move(data));
            }
            queue_cv_.notify_one();
        }
    } else if (transfer->status == LIBUSB_TRANSFER_CANCELLED) {
        // Disconnect()가 요청한 취소 — 완료 플래그 설정 후 종료
        read_transfer_complete_.store(true);
        transfer_cv_.notify_one();
        return;
    } else {
        AA_LOG_E() << "Async Read 실패: " << transfer->status;
        if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE || transfer->status == LIBUSB_TRANSFER_ERROR) {
            AA_LOG_E() << "치명적 Read 에러 감지. 강제 연결 종료 처리.";
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
