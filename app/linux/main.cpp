#define LOG_TAG "Main"
#include <QApplication>
#include <csignal>
#include <memory>

#include "aauto/core/AAutoEngine.hpp"
#include "aauto/core/DeviceManager.hpp"
#include "aauto/core/HeadunitConfig.hpp"
#include "aauto/hw/UsbDeviceDetector.hpp"
#include "aauto/platform/IPlatform.hpp"
#include "aauto/platform/qt/QtPlatform.hpp"
#include "aauto/utils/Logger.hpp"
#include <cstdlib>
#include <string>
#include <unistd.h>

static aauto::platform::IPlatform* g_platform = nullptr;

static void OnSignal(int) {
    if (g_platform) g_platform->Stop();
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    // Set PipeWire/PulseAudio socket path before gst_init().
    // When running under sudo, getuid()=0, so use SUDO_UID to get the real user UID.
    const char* sudo_uid_str = getenv("SUDO_UID");
    uid_t real_uid = sudo_uid_str ? static_cast<uid_t>(std::stoul(sudo_uid_str)) : getuid();
    std::string runtime_dir = "/run/user/" + std::to_string(real_uid);
    setenv("XDG_RUNTIME_DIR",      runtime_dir.c_str(),                               1);
    setenv("PULSE_SERVER",         ("unix:" + runtime_dir + "/pulse/native").c_str(), 1);
    setenv("PIPEWIRE_RUNTIME_DIR", runtime_dir.c_str(),                               1);

    AA_LOG_I() << "=== Android Auto (AAuto) engine initializing ===";

    // 1. Initialize platform (Qt5 + ALSA)
    auto platform = std::make_shared<aauto::platform::qt::QtPlatform>();
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
    aauto::core::AAutoEngine engine(device_manager, platform, config);
    engine.Initialize();

    // 4. Start USB device detection
    auto detector = std::make_shared<aauto::hw::UsbDeviceDetector>(device_manager);
    if (!detector->Init() || !detector->Start()) {
        AA_LOG_E() << "Device detector failed to start";
        return -1;
    }
    AA_LOG_I() << "Connect a USB device. (Close window or Ctrl+C to quit)";

    // 5. Qt event loop
    platform->Run();

    // 6. Ordered shutdown
    detector->Stop();
    AA_LOG_I() << "=== Shutdown complete ===";
    return 0;
}
