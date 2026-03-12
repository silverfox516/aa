#pragma once

#include <libusb.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "aauto/core/DeviceManager.hpp"
#include "aauto/hw/IDeviceDetector.hpp"

namespace aauto {
namespace hw {

constexpr uint16_t GOOGLE_VID   = 0x18D1;
constexpr uint16_t AOA_PID_MIN  = 0x2D00;
constexpr uint16_t AOA_PID_MAX  = 0x2D05;

class UsbDeviceDetector : public IDeviceDetector {
   public:
    explicit UsbDeviceDetector(core::DeviceManager& device_manager);
    ~UsbDeviceDetector() override;

    bool Init() override;
    bool Start() override;
    void Stop() override;

   private:
    static int LIBUSB_CALL HotplugCallback(libusb_context* ctx, libusb_device* device,
                                           libusb_hotplug_event event, void* user_data);

    void HandleDeviceConnected(libusb_device* device);
    void HandleDeviceDisconnected(libusb_device* device);

    struct DeviceEvent {
        enum class Type { CONNECTED, DISCONNECTED };
        Type type;
        libusb_device* device;
    };

    void ProcessEventsLoop();
    void ProcessDeviceConnected(libusb_device* device);
    void ProcessDeviceDisconnected(libusb_device* device);

    bool TrySwitchToAccessoryMode(libusb_device* device, libusb_device_handle* handle);
    bool SendAoaString(libusb_device_handle* handle, uint16_t index, const std::string& str);

    bool IsAccessoryDevice(uint16_t vid, uint16_t pid);
    bool IsPotentialAndroidDevice(uint16_t vid, uint16_t pid);

   private:
    core::DeviceManager& device_manager_;

    libusb_context* ctx_;
    libusb_hotplug_callback_handle callback_handle_;
    std::atomic<bool> is_running_;
    std::thread event_thread_;
    std::thread process_thread_;

    std::queue<DeviceEvent> event_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    std::map<libusb_device*, std::string> connected_devices_;
    std::mutex map_mutex_;
};

} // namespace hw
} // namespace aauto
