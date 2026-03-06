#include "aauto/hw/UsbDeviceDetector.hpp"

#include <iostream>
#include <map>
#include <mutex>

#include "aauto/core/DeviceManager.hpp"
#include "aauto/transport/UsbTransport.hpp"

namespace aauto {
namespace hw {

// AOA 상수들은 UsbDeviceDetector.hpp 에 정의되어 있음.


UsbDeviceDetector::UsbDeviceDetector()
    : ctx_(nullptr), callback_handle_(0), is_running_(false) {}

UsbDeviceDetector::~UsbDeviceDetector() { Stop(); }

bool UsbDeviceDetector::Init() {
    int rc = libusb_init(&ctx_);
    if (rc < 0) {
        std::cerr << "[UsbDetector] libusb 초기화 실패: " << libusb_error_name(rc) << std::endl;
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
        std::cerr << "[UsbDetector] Hotplug 콜백 등록 실패: " << libusb_error_name(rc) << std::endl;
        return false;
    }

    is_running_ = true;
    event_thread_ = std::thread([this]() {
        while (is_running_) {
            // Hotplug 이벤트를 비동기로 계속 처리
            libusb_handle_events_completed(ctx_, nullptr);
        }
    });

    std::cout << "[UsbDetector] 디바이스 연결 감지를 시작합니다..." << std::endl;
    return true;
}

void UsbDeviceDetector::Stop() {
    if (is_running_) {
        is_running_ = false;
        if (event_thread_.joinable()) {
            // libusb_handle_events_completed 탈출을 위해 컨텍스트 인터럽트 유도 (간소화)
            event_thread_.join();
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
    // 모든 장치를 대상으로 AOA 질의를 하거나, 알려진 Android 벤더 ID 목록을 사용할 수 있습니다.
    // 여기서는 AOA 모드가 아닌 장치를 Potential 장치로 취급합니다.
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

void UsbDeviceDetector::HandleDeviceConnected(libusb_device* device) {
    libusb_device_descriptor desc;
    if (libusb_get_device_descriptor(device, &desc) < 0) return;

    std::string device_id = "usb_" + std::to_string(desc.idVendor) + "_" + std::to_string(desc.idProduct);

    if (IsAccessoryDevice(desc.idVendor, desc.idProduct)) {
        std::cout << "[UsbDetector] AOA 장치 인식 완료 (Ready for Android Auto): " << device_id << std::endl;
        
        libusb_device_handle* handle = nullptr;
        if (libusb_open(device, &handle) == 0) {
            {
                std::lock_guard<std::mutex> lock(map_mutex_);
                connected_devices_[device] = device_id;
            }
            
            // Transport 생성 및 DeviceManager로 이관
            auto transport = std::make_shared<aauto::transport::UsbTransport>(handle);
            aauto::transport::DeviceInfo info = {device_id, "Android Open Accessory Device", aauto::transport::TransportType::USB};
            aauto::core::DeviceManager::GetInstance().NotifyDeviceConnected(info, transport);
        } else {
            std::cerr << "[UsbDetector] AOA 장치를 열 수 없습니다." << std::endl;
        }

    } else if (IsPotentialAndroidDevice(desc.idVendor, desc.idProduct)) {
        std::cout << "[UsbDetector] 일반 USB 기기 감지됨. AOA 지원 여부 및 전환 시도: " << device_id << std::endl;
        
        libusb_device_handle* handle = nullptr;
        if (libusb_open(device, &handle) == 0) {
            TrySwitchToAccessoryMode(device, handle);
            libusb_close(handle);
        }
    }
}

void UsbDeviceDetector::HandleDeviceDisconnected(libusb_device* device) {
    std::string device_id;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        auto it = connected_devices_.find(device);
        if (it != connected_devices_.end()) {
            device_id = it->second;
            connected_devices_.erase(it);
        }
    }

    if (!device_id.empty()) {
        std::cout << "[UsbDetector] AOA 장치 연결 해제됨: " << device_id << std::endl;
        aauto::core::DeviceManager::GetInstance().NotifyDeviceDisconnected(device_id);
    } else {
        std::cout << "[UsbDetector] 일반 장치 연결 해제됨 (또는 AOA 모드로 재부팅 중)" << std::endl;
    }
}

bool UsbDeviceDetector::SendAoaString(libusb_device_handle* handle, uint16_t index, const std::string& str) {
    int rc = libusb_control_transfer(
        handle,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
        AOA_SEND_STRING,
        0, index,
        (unsigned char*)str.c_str(), str.length() + 1,
        1000 // timeout ms
    );
    return rc >= 0;
}

bool UsbDeviceDetector::TrySwitchToAccessoryMode(libusb_device* device, libusb_device_handle* handle) {
    unsigned char ioBuffer[2];
    
    // 1단계: AOA 프로토콜 버전 확인 질의
    int protocol = -1;
    int rc = libusb_control_transfer(
        handle,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR,
        AOA_GET_PROTOCOL,
        0, 0,
        ioBuffer, 2,
        1000
    );

    if (rc >= 2) {
        protocol = ioBuffer[0] | (ioBuffer[1] << 8);
    }
    
    if (protocol < 1 || protocol > 2) {
        // AOA를 지원하지 않는 기기
        return false;
    }

    std::cout << "[UsbDetector] 장치가 AOA 프로토콜 버전 " << protocol << "을 지원합니다." << std::endl;

    // 2단계: Accessory 식별 정보 전송
    SendAoaString(handle, AOA_STRING_MANUFACTURER, AAUTO_MANUFACTURER);
    SendAoaString(handle, AOA_STRING_MODEL, AAUTO_MODEL);
    SendAoaString(handle, AOA_STRING_DESCRIPTION, AAUTO_DESCRIPTION);
    SendAoaString(handle, AOA_STRING_VERSION, AAUTO_VERSION);
    SendAoaString(handle, AOA_STRING_URI, AAUTO_URI);
    SendAoaString(handle, AOA_STRING_SERIAL, AAUTO_SERIAL);

    // 3단계: Accessory 모드 전환 명령 전송
    std::cout << "[UsbDetector] 장치 액세서리 모드 스위칭 명령 전송!" << std::endl;
    rc = libusb_control_transfer(
        handle,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
        AOA_START,
        0, 0,
        nullptr, 0,
        1000
    );

    return rc >= 0;
}

}  // namespace hw
}  // namespace aauto
