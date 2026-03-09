#define LOG_TAG "UsbDetector"
#include "aauto/hw/UsbDeviceDetector.hpp"

#include <map>
#include <mutex>

#include "aauto/core/DeviceManager.hpp"
#include "aauto/transport/UsbTransport.hpp"
#include "aauto/utils/Logger.hpp"

namespace aauto {
namespace hw {

// AA 상수들은 UsbDeviceDetector.hpp 에 정의되어 있음.


UsbDeviceDetector::UsbDeviceDetector()
    : ctx_(nullptr), callback_handle_(0), is_running_(false) {}

UsbDeviceDetector::~UsbDeviceDetector() { Stop(); }

bool UsbDeviceDetector::Init() {
    int rc = libusb_init(&ctx_);
    if (rc != 0) {
        AA_LOG_E() << "libusb_init 실패: " << libusb_error_name(rc);
        return false;
    }

    return true;
}

bool UsbDeviceDetector::Start() {
    if (!ctx_) return false;

    // Hotplug 콜백 등록 (모든 벤더, 기기, 클래스 매칭)
    int rc = libusb_hotplug_register_callback(
        ctx_, static_cast<libusb_hotplug_event>(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
        LIBUSB_HOTPLUG_ENUMERATE, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY,
        UsbDeviceDetector::HotplugCallback, this, &callback_handle_);

    if (rc != LIBUSB_SUCCESS) {
        AA_LOG_E() << "Hotplug 콜백 등록 실패: " << libusb_error_name(rc);
        return false;
    }

    is_running_ = true;
    event_thread_ = std::thread([this]() {
        timeval libusbEventTimeout{180, 0};
        while (is_running_) {
            // Hotplug 이벤트를 비동기로 계속 처리
            libusb_handle_events_timeout_completed(ctx_, &libusbEventTimeout, nullptr);
        }
    });
    process_thread_ = std::thread(&UsbDeviceDetector::ProcessEventsLoop, this);

    AA_LOG_I() << "디바이스 연결 감지를 시작합니다...";
    return true;
}

void UsbDeviceDetector::Stop() {
    if (is_running_) {
        is_running_ = false;
        queue_cv_.notify_all(); // Wake up process thread
        if (event_thread_.joinable()) {
            // libusb_handle_events_completed 탈출을 위해 컨텍스트 인터럽트 유도 (간소화)
            event_thread_.join();
        }
        if (process_thread_.joinable()) {
            process_thread_.join();
        }
    }

    if (ctx_) {
        if (callback_handle_ > 0) {
            libusb_hotplug_deregister_callback(ctx_, callback_handle_);
            callback_handle_ = 0;
        }
        libusb_exit(ctx_);
        ctx_ = nullptr;
    }
}

bool UsbDeviceDetector::IsAccessoryDevice(uint16_t vid, uint16_t pid) {
    return (vid == GOOGLE_VID) && (pid >= AOA_PID_MIN && pid <= AOA_PID_MAX);
}

bool UsbDeviceDetector::IsPotentialAndroidDevice(uint16_t vid, uint16_t pid) {
    // 모든 장치를 대상으로 AA 질의를 하거나, 알려진 Android 벤더 ID 목록을 사용할 수 있습니다.
    // 여기서는 AA 모드가 아닌 장치를 Potential 장치로 취급합니다.
    return !IsAccessoryDevice(vid, pid);
}

int LIBUSB_CALL UsbDeviceDetector::HotplugCallback(libusb_context* ctx, libusb_device* device,
                                                   libusb_hotplug_event event, void* user_data) {
    auto* detector = static_cast<UsbDeviceDetector*>(user_data);

    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) {
        detector->HandleDeviceConnected(device);
    } else if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT) {
        detector->HandleDeviceDisconnected(device);
    }

    return 0;
}

void UsbDeviceDetector::ProcessEventsLoop() {
    while (is_running_) {
        DeviceEvent event;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { return !event_queue_.empty() || !is_running_; });

            if (!is_running_ && event_queue_.empty()) {
                break;
            }

            event = event_queue_.front();
            event_queue_.pop();
        }

        if (event.type == DeviceEvent::Type::CONNECTED) {
            ProcessDeviceConnected(event.device);
        } else if (event.type == DeviceEvent::Type::DISCONNECTED) {
            ProcessDeviceDisconnected(event.device);
        }

        libusb_unref_device(event.device);
    }
}

void UsbDeviceDetector::HandleDeviceConnected(libusb_device* device) {
    libusb_ref_device(device);
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        event_queue_.push({DeviceEvent::Type::CONNECTED, device});
    }
    queue_cv_.notify_one();
}

void UsbDeviceDetector::HandleDeviceDisconnected(libusb_device* device) {
    libusb_ref_device(device);
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        event_queue_.push({DeviceEvent::Type::DISCONNECTED, device});
    }
    queue_cv_.notify_one();
}

void UsbDeviceDetector::ProcessDeviceConnected(libusb_device* device) {
    libusb_device_descriptor desc;
    if (libusb_get_device_descriptor(device, &desc) < 0) {
        return;
    }

    char id_buf[32];
    snprintf(id_buf, sizeof(id_buf), "usb_%04x_%04x", desc.idVendor, desc.idProduct);
    std::string device_id(id_buf);

    // 기본 Descriptor 정보 출력: 버스, 디바이스, VID, PID
    uint8_t bus = libusb_get_bus_number(device);
    uint8_t dev_addr = libusb_get_device_address(device);
    
    char info_buf[128];
    snprintf(info_buf, sizeof(info_buf), "Bus: %03d, Device: %03d, VID: %04x, PID: %04x",
             bus, dev_addr, desc.idVendor, desc.idProduct);
    AA_LOG_I() << "[+] USB 장치 연결됨 - " << info_buf;

    if (IsAccessoryDevice(desc.idVendor, desc.idProduct)) {
        AA_LOG_I() << "🚀 AOA 장치 인식 완료 (Ready for Android Auto): " << device_id;

        libusb_device_handle* handle = nullptr;
        if (libusb_open(device, &handle) == 0) {
            AA_LOG_I() << "🔓 USB 핸들 오픈 (AOA) - Handle: " << handle;
            {
                std::lock_guard<std::mutex> lock(map_mutex_);
                connected_devices_[device] = device_id;
            }

            // Transport 생성 및 DeviceManager로 이관
            auto transport = std::make_shared<aauto::transport::UsbTransport>(handle);
            aauto::transport::DeviceInfo info = {device_id, "Android Open Accessory Device", aauto::transport::TransportType::USB};
            aauto::core::DeviceManager::GetInstance().NotifyDeviceConnected(info, transport);
        } else {
            AA_LOG_E() << "❌ AOA 장치를 열 수 없습니다.";
        }

    } else if (IsPotentialAndroidDevice(desc.idVendor, desc.idProduct)) {
        AA_LOG_I() << "🔍 일반 USB 기기 감지됨. AOA 지원 여부 및 전환 시도: " << device_id;

        libusb_device_handle* handle = nullptr;
        if (libusb_open(device, &handle) == 0) {
            AA_LOG_I() << "🔓 USB 핸들 오픈 (Switching) - Handle: " << handle;
            TrySwitchToAccessoryMode(device, handle);
            AA_LOG_I() << "🔒 USB 핸들 클로즈 (Switching) - Handle: " << handle;
            libusb_close(handle);
        }
    }
}

void UsbDeviceDetector::ProcessDeviceDisconnected(libusb_device* device) {
    libusb_device_descriptor desc;
    if (libusb_get_device_descriptor(device, &desc) == 0) {
        uint8_t bus = libusb_get_bus_number(device);
        uint8_t dev_addr = libusb_get_device_address(device);
        char info_buf[128];
        snprintf(info_buf, sizeof(info_buf), "Bus: %03d, Device: %03d, VID: %04x, PID: %04x",
                 bus, dev_addr, desc.idVendor, desc.idProduct);
        AA_LOG_I() << "[-] USB 장치 해제됨 - " << info_buf;
    }

    std::string device_id;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        auto it = connected_devices_.find(device);
        if (it != connected_devices_.end()) {
            device_id = it->second;
            AA_LOG_I() << "🔍 장치 관리 맵에서 해제 대상 발견: " << device_id;
            connected_devices_.erase(it);
        }
    }

    if (!device_id.empty()) {
        AA_LOG_I() << "AA 장치 세션 연결 해제: " << device_id;
        aauto::core::DeviceManager::GetInstance().NotifyDeviceDisconnected(device_id);
    }
}

struct ControlContext {
    bool completed = false;
    int result = 0;
    std::mutex mtx;
    std::condition_variable cv;
};

static void LIBUSB_CALL OnControlTransferComplete(struct libusb_transfer* transfer) {
    ControlContext* ctx = static_cast<ControlContext*>(transfer->user_data);
    std::lock_guard<std::mutex> lock(ctx->mtx);
    ctx->completed = true;
    ctx->result = (transfer->status == LIBUSB_TRANSFER_COMPLETED) ? transfer->actual_length : -1;
    ctx->cv.notify_one();
}

static int ControlTransferSync(libusb_device_handle* handle, uint8_t request_type, uint8_t bRequest,
                              uint16_t wValue, uint16_t wIndex, unsigned char* data, uint16_t wLength, uint32_t timeout) {
    struct libusb_transfer* xfer = libusb_alloc_transfer(0);
    if (!xfer) return -1;

    std::vector<uint8_t> buffer(LIBUSB_CONTROL_SETUP_SIZE + wLength);
    libusb_fill_control_setup(buffer.data(), request_type, bRequest, wValue, wIndex, wLength);
    if (wLength > 0 && (request_type & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_OUT) {
        std::copy(data, data + wLength, buffer.data() + LIBUSB_CONTROL_SETUP_SIZE);
    }

    ControlContext ctx;
    libusb_fill_control_transfer(xfer, handle, buffer.data(), OnControlTransferComplete, &ctx, timeout);

    int rc = libusb_submit_transfer(xfer);
    if (rc != 0) {
        libusb_free_transfer(xfer);
        return rc;
    }

    {
        std::unique_lock<std::mutex> lock(ctx.mtx);
        ctx.cv.wait(lock, [&ctx] { return ctx.completed; });
    }

    int result = ctx.result;
    if (result >= 0 && wLength > 0 && (request_type & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN) {
        std::copy(buffer.begin() + LIBUSB_CONTROL_SETUP_SIZE, buffer.begin() + LIBUSB_CONTROL_SETUP_SIZE + result, data);
    }

    libusb_free_transfer(xfer);
    return result;
}

bool UsbDeviceDetector::SendAoaString(libusb_device_handle* handle, uint16_t index, const std::string& str) {
    int rc = ControlTransferSync(
        handle,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
        AOA_SEND_STRING,
        0, index,
        (unsigned char*)str.c_str(), str.length() + 1,
        1000
    );
    return rc >= 0;
}

bool UsbDeviceDetector::TrySwitchToAccessoryMode(libusb_device* device, libusb_device_handle* handle) {
    unsigned char ioBuffer[2];

    // 1단계: AA 프로토콜 버전 확인 질의
    int protocol = -1;
    int rc = ControlTransferSync(
        handle,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR,
        AOA_GET_PROTOCOL,
        0, 0,
        ioBuffer, 2,
        1000
    );

    if (rc >= 2) {
        protocol = ioBuffer[0] | (ioBuffer[1] << 8);
    } else {
        AA_LOG_E() << "❌ AOA 프로토콜 정보 요청 실패 (rc=" << rc << ")";
        return false;
    }

    if (protocol < 1 || protocol > 2) {
        // AA를 지원하지 않는 기기
        AA_LOG_E() << "❌ 기기가 AOA 프로토콜을 지원하지 않음 (protocol=" << protocol << ")";
        return false;
    }

    AA_LOG_I() << "✅ 장치가 AA 프로토콜 버전 " << protocol << "을 지원합니다.";

    // 2단계: Accessory 식별 정보 전송
    if (!SendAoaString(handle, AOA_STRING_MANUFACTURER, AAUTO_MANUFACTURER)) AA_LOG_W() << "⚠️ MANUFACTURER 전송 실패";
    if (!SendAoaString(handle, AOA_STRING_MODEL, AAUTO_MODEL)) AA_LOG_W() << "⚠️ MODEL 전송 실패";
    if (!SendAoaString(handle, AOA_STRING_DESCRIPTION, AAUTO_DESCRIPTION)) AA_LOG_W() << "⚠️ DESCRIPTION 전송 실패";
    if (!SendAoaString(handle, AOA_STRING_VERSION, AAUTO_VERSION)) AA_LOG_W() << "⚠️ VERSION 전송 실패";
    if (!SendAoaString(handle, AOA_STRING_URI, AAUTO_URI)) AA_LOG_W() << "⚠️ URI 전송 실패";
    if (!SendAoaString(handle, AOA_STRING_SERIAL, AAUTO_SERIAL)) AA_LOG_W() << "⚠️ SERIAL 전송 실패";

    // 3단계: Accessory 모드 전환 명령 전송
    AA_LOG_I() << "장치 액세서리 모드 스위칭 명령 전송 시도...";
    rc = ControlTransferSync(
        handle,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
        AOA_START,
        0, 0,
        nullptr, 0,
        1000
    );

    if (rc < 0) {
        AA_LOG_E() << "❌ 장치 액세서리 모드 스위칭 명령 실패 (rc=" << rc << ")";
        return false;
    }

    AA_LOG_I() << "✅ 장치 액세서리 모드 스위칭 명령 전송 성공!";
    return true;
}

}  // namespace hw
}  // namespace aauto
