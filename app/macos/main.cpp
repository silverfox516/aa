#define LOG_TAG "Main"
#include <csignal>
#include <memory>

#include "aauto/core/AAutoEngine.hpp"
#include "aauto/core/DeviceManager.hpp"
#include "aauto/core/HeadunitConfig.hpp"
#include "aauto/hw/UsbDeviceDetector.hpp"
#include "aauto/platform/IPlatform.hpp"
#include "aauto/platform/sdl2/Sdl2Platform.hpp"
#include "aauto/utils/Logger.hpp"

static aauto::platform::IPlatform* g_platform = nullptr;

static void OnSignal(int) {
    if (g_platform) g_platform->Stop();
}

int main() {
    AA_LOG_I() << "=== Android Auto (AAuto) engine initializing ===";

    // 1. Select and initialize platform
    auto platform = std::make_shared<aauto::platform::sdl2::Sdl2Platform>();
    if (!platform->Initialize()) {
        AA_LOG_E() << "Platform initialization failed";
        return -1;
    }

    // 2. Register shutdown signal handlers
    g_platform = platform.get();
    std::signal(SIGINT,  OnSignal);
    std::signal(SIGTERM, OnSignal);

    // 3. Build engine
    aauto::core::DeviceManager device_manager;
    aauto::core::HeadunitConfig config;
    auto detector = std::make_shared<aauto::hw::UsbDeviceDetector>(device_manager);
    if (!detector->Init()) {
        AA_LOG_E() << "Device detector initialization failed";
        return -1;
    }

    // 4. Wrap engine in inner scope to guarantee shutdown order:
    //    engine (transport) destroyed -> Disconnect() completes -> detector StopEventLoop()
    //    (detector event_thread must remain alive to process cancel callbacks)
    {
        aauto::core::AAutoEngine engine(device_manager, platform, config);
        engine.Initialize();

        if (!detector->Start()) {
            AA_LOG_E() << "Device detector failed to start";
            return -1;
        }
        AA_LOG_I() << "Connect a USB device. (ESC or Ctrl+C to quit)";

        // 5. Main thread: platform event loop
        platform->Run();
    }

    // 6. After engine is destroyed, stop event thread then let detector destructor call libusb_exit
    detector->StopEventLoop();

    AA_LOG_I() << "=== Shutdown complete ===";
    return 0;
}
