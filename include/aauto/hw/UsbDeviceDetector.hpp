#pragma once

#include <libusb.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <map> // Added for std::map
#include <mutex> // Added for std::mutex

#include "aauto/hw/IDeviceDetector.hpp"

namespace aauto {
namespace hw {

// Google VID 및 Android Open Accessory (AOA) PID 범위
constexpr uint16_t GOOGLE_VID = 0x18D1;
constexpr uint16_t AOA_PID_MIN = 0x2D00; // Accessory
constexpr uint16_t AOA_PID_MAX = 0x2D05; // Accessory + ADB + Audio 등

// AOA Protocol 제어 전송(Control Transfer) 정의
constexpr uint8_t AOA_GET_PROTOCOL = 51;
constexpr uint8_t AOA_SEND_STRING = 52;
constexpr uint8_t AOA_START = 53;

// AOA 문자열 ID 정의
constexpr uint16_t AOA_STRING_MANUFACTURER = 0;
constexpr uint16_t AOA_STRING_MODEL = 1;
constexpr uint16_t AOA_STRING_DESCRIPTION = 2;
constexpr uint16_t AOA_STRING_VERSION = 3;
constexpr uint16_t AOA_STRING_URI = 4;
constexpr uint16_t AOA_STRING_SERIAL = 5;

// Android Auto 관련 정보
const std::string AAUTO_MANUFACTURER = "aauto_project";
const std::string AAUTO_MODEL = "aauto_headunit";
const std::string AAUTO_DESCRIPTION = "Android Auto Headunit Simulator";
const std::string AAUTO_VERSION = "1.0";
const std::string AAUTO_URI = "https://github.com/aauto";
const std::string AAUTO_SERIAL = "00000001";

class UsbDeviceDetector : public IDeviceDetector {
   public:
    UsbDeviceDetector();
    ~UsbDeviceDetector() override;

    bool Init() override;
    bool Start() override;
    void Stop() override;

   private:
    static int LIBUSB_CALL HotplugCallback(libusb_context* ctx, libusb_device* device, libusb_hotplug_event event,
                                           void* user_data);

    void HandleDeviceConnected(libusb_device* device);
    void HandleDeviceDisconnected(libusb_device* device);

    // AOA 모드 전환 프로세스 수행
    bool TrySwitchToAccessoryMode(libusb_device* device, libusb_device_handle* handle);
    bool SendAoaString(libusb_device_handle* handle, uint16_t index, const std::string& str);

    // 기존 폰(MTP/PTP 등)인지, AOA 상태로 재부팅된 장치인지 판별
    bool IsAccessoryDevice(uint16_t vid, uint16_t pid);
    bool IsPotentialAndroidDevice(uint16_t vid, uint16_t pid);

   private:
    libusb_context* ctx_;
    libusb_hotplug_callback_handle callback_handle_;
    std::atomic<bool> is_running_;
    std::thread event_thread_;

    // 관리 중인 기기 목록 (Device Pointer -> Device ID)
    std::map<libusb_device*, std::string> connected_devices_;
    std::mutex map_mutex_;
};

}  // namespace hw
}  // namespace aauto
