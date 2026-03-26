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

    // gst_init() 이전에 PipeWire/PulseAudio 소켓 경로 설정
    // sudo 실행 시 getuid()=0이므로 SUDO_UID로 실제 사용자 UID를 가져옴
    const char* sudo_uid_str = getenv("SUDO_UID");
    uid_t real_uid = sudo_uid_str ? static_cast<uid_t>(std::stoul(sudo_uid_str)) : getuid();
    std::string runtime_dir = "/run/user/" + std::to_string(real_uid);
    setenv("XDG_RUNTIME_DIR",      runtime_dir.c_str(),                               1);
    setenv("PULSE_SERVER",         ("unix:" + runtime_dir + "/pulse/native").c_str(), 1);
    setenv("PIPEWIRE_RUNTIME_DIR", runtime_dir.c_str(),                               1);

    AA_LOG_I() << "=== Android Auto (AAuto) 엔진 초기화 ===";

    // 1. Platform 초기화 (Qt5 + ALSA)
    auto platform = std::make_shared<aauto::platform::qt::QtPlatform>();
    if (!platform->Initialize()) {
        AA_LOG_E() << "Platform 초기화 실패";
        return -1;
    }

    // 2. 종료 시그널 핸들러
    g_platform = platform.get();
    std::signal(SIGINT,  OnSignal);
    std::signal(SIGTERM, OnSignal);

    // 3. 엔진 구성
    aauto::core::DeviceManager device_manager;
    aauto::core::HeadunitConfig config;
    aauto::core::AAutoEngine engine(device_manager, platform, config);
    engine.Initialize();

    // 4. USB 디바이스 감지 시작
    auto detector = std::make_shared<aauto::hw::UsbDeviceDetector>(device_manager);
    if (!detector->Init() || !detector->Start()) {
        AA_LOG_E() << "디바이스 감지기 구동 실패";
        return -1;
    }
    AA_LOG_I() << "USB 장치를 연결하세요. (창 닫기 또는 Ctrl+C 로 종료)";

    // 5. Qt 이벤트 루프
    platform->Run();

    // 6. 순차 종료
    detector->Stop();
    AA_LOG_I() << "=== 프로그램 종료 ===";
    return 0;
}
