#define LOG_TAG "AA.UsbDetector"
#include "aauto/hw/UsbDeviceDetector.hpp"

#include <map>
#include <mutex>

#include "aauto/core/DeviceManager.hpp"
#include "aauto/transport/UsbTransport.hpp"
#include "aauto/utils/Logger.hpp"

namespace aauto {
namespace hw {

// AOA protocol control request codes
constexpr uint8_t  AOA_GET_PROTOCOL = 51;
constexpr uint8_t  AOA_SEND_STRING  = 52;
constexpr uint8_t  AOA_START        = 53;

// AOA string indices
constexpr uint16_t AOA_STRING_MANUFACTURER = 0;
constexpr uint16_t AOA_STRING_MODEL        = 1;
constexpr uint16_t AOA_STRING_DESCRIPTION  = 2;
constexpr uint16_t AOA_STRING_VERSION      = 3;
constexpr uint16_t AOA_STRING_URI          = 4;
constexpr uint16_t AOA_STRING_SERIAL       = 5;

// Android Auto accessory identification strings
static const std::string AAUTO_MANUFACTURER = "Android";
static const std::string AAUTO_MODEL        = "Android Auto";
static const std::string AAUTO_DESCRIPTION  = "Android Auto";
static const std::string AAUTO_VERSION      = "2.0.1";
static const std::string AAUTO_URI          = "https://developer.android.com/auto/index.html";
static const std::string AAUTO_SERIAL       = "HU-AAAAAA001";


UsbDeviceDetector::UsbDeviceDetector(core::DeviceManager& device_manager)
    : device_manager_(device_manager), ctx_(nullptr), callback_handle_(0), is_running_(false) {}

UsbDeviceDetector::~UsbDeviceDetector() { Stop(); }

bool UsbDeviceDetector::Init() {
    int rc = libusb_init(&ctx_);
    if (rc != 0) {
        AA_LOG_E() << "libusb_init failed: " << libusb_error_name(rc);
        return false;
    }

    return true;
}

bool UsbDeviceDetector::Start() {
    if (!ctx_) return false;

    // Register hotplug callback for all vendors, devices, and classes
    int rc = libusb_hotplug_register_callback(
        ctx_, static_cast<libusb_hotplug_event>(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
        LIBUSB_HOTPLUG_ENUMERATE, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY,
        UsbDeviceDetector::HotplugCallback, this, &callback_handle_);

    if (rc != LIBUSB_SUCCESS) {
        AA_LOG_E() << "Hotplug callback registration failed: " << libusb_error_name(rc);
        return false;
    }

    is_running_ = true;
    event_thread_ = std::thread([this]() {
        timeval libusbEventTimeout{180, 0};
        while (is_running_) {
            // Process hotplug events asynchronously
            libusb_handle_events_timeout_completed(ctx_, &libusbEventTimeout, nullptr);
        }
    });
    process_thread_ = std::thread(&UsbDeviceDetector::ProcessEventsLoop, this);

    AA_LOG_I() << "USB device detection started";
    return true;
}

void UsbDeviceDetector::StopEventLoop() {
    if (is_running_) {
        is_running_ = false;
        queue_cv_.notify_all();
        if (ctx_) libusb_interrupt_event_handler(ctx_);
        if (event_thread_.joinable()) event_thread_.join();
        if (process_thread_.joinable()) process_thread_.join();
    }
}

void UsbDeviceDetector::Stop() {
    StopEventLoop();

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
    // Treat any non-AOA device as a potential Android device and attempt AOA negotiation
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

    uint8_t bus = libusb_get_bus_number(device);
    uint8_t dev_addr = libusb_get_device_address(device);

    char info_buf[128];
    snprintf(info_buf, sizeof(info_buf), "Bus: %03d, Device: %03d, VID: %04x, PID: %04x",
             bus, dev_addr, desc.idVendor, desc.idProduct);
    AA_LOG_I() << "[+] USB device connected - " << info_buf;

    if (IsAccessoryDevice(desc.idVendor, desc.idProduct)) {
        AA_LOG_I() << "AOA device ready (Android Auto): " << device_id;

        libusb_device_handle* handle = nullptr;
        if (libusb_open(device, &handle) == 0) {
            AA_LOG_I() << "USB handle opened (AOA) - handle: " << handle;
            {
                std::lock_guard<std::mutex> lock(map_mutex_);
                connected_devices_[device] = device_id;
            }

            // Hand the transport off to the DeviceManager
            auto transport = std::make_shared<aauto::transport::UsbTransport>(handle, ctx_);
            aauto::transport::DeviceInfo info = {device_id, "Android Open Accessory Device", aauto::transport::TransportType::USB};
            device_manager_.NotifyDeviceConnected(info, transport);
        } else {
            AA_LOG_E() << "Failed to open AOA device";
        }

    } else if (IsPotentialAndroidDevice(desc.idVendor, desc.idProduct)) {
        AA_LOG_I() << "Generic USB device detected, attempting AOA switch: " << device_id;

        libusb_device_handle* handle = nullptr;
        if (libusb_open(device, &handle) == 0) {
            AA_LOG_I() << "USB handle opened (switching) - handle: " << handle;
            TrySwitchToAccessoryMode(device, handle);
            AA_LOG_I() << "USB handle closed (switching) - handle: " << handle;
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
        AA_LOG_I() << "[-] USB device disconnected - " << info_buf;
    }

    std::string device_id;
    {
        std::lock_guard<std::mutex> lock(map_mutex_);
        auto it = connected_devices_.find(device);
        if (it != connected_devices_.end()) {
            device_id = it->second;
            AA_LOG_I() << "Removing device from map: " << device_id;
            connected_devices_.erase(it);
        }
    }

    if (!device_id.empty()) {
        AA_LOG_I() << "AA device session disconnected: " << device_id;
        device_manager_.NotifyDeviceDisconnected(device_id);
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

    // Step 1: Query AOA protocol version
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
        AA_LOG_E() << "AOA protocol query failed (rc=" << rc << ")";
        return false;
    }

    if (protocol < 1 || protocol > 2) {
        AA_LOG_E() << "Device does not support AOA protocol (protocol=" << protocol << ")";
        return false;
    }

    AA_LOG_I() << "Device supports AOA protocol version " << protocol;

    // Step 2: Send accessory identification strings
    if (!SendAoaString(handle, AOA_STRING_MANUFACTURER, AAUTO_MANUFACTURER)) AA_LOG_W() << "MANUFACTURER send failed";
    if (!SendAoaString(handle, AOA_STRING_MODEL, AAUTO_MODEL))               AA_LOG_W() << "MODEL send failed";
    if (!SendAoaString(handle, AOA_STRING_DESCRIPTION, AAUTO_DESCRIPTION))   AA_LOG_W() << "DESCRIPTION send failed";
    if (!SendAoaString(handle, AOA_STRING_VERSION, AAUTO_VERSION))           AA_LOG_W() << "VERSION send failed";
    if (!SendAoaString(handle, AOA_STRING_URI, AAUTO_URI))                   AA_LOG_W() << "URI send failed";
    if (!SendAoaString(handle, AOA_STRING_SERIAL, AAUTO_SERIAL))             AA_LOG_W() << "SERIAL send failed";

    // Step 3: Send accessory mode switch command
    AA_LOG_I() << "Sending accessory mode switch command...";
    rc = ControlTransferSync(
        handle,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
        AOA_START,
        0, 0,
        nullptr, 0,
        1000
    );

    if (rc < 0) {
        AA_LOG_E() << "Accessory mode switch command failed (rc=" << rc << ")";
        return false;
    }

    AA_LOG_I() << "Accessory mode switch command sent successfully";
    return true;
}

}  // namespace hw
}  // namespace aauto
