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
    AA_LOG_I() << "=== Android Auto (AAuto) 엔진 초기화 ===";

    // 1. Platform 선택 및 초기화 (여기서 SDL2 → Qt / Flutter 로 교체 가능)
    auto platform = std::make_shared<aauto::platform::sdl2::Sdl2Platform>();
    if (!platform->Initialize()) {
        AA_LOG_E() << "Platform 초기화 실패";
        return -1;
    }

    // 2. 종료 시그널 핸들러
    g_platform = platform.get();
    std::signal(SIGINT,  OnSignal);
    std::signal(SIGTERM, OnSignal);

    // 3. 엔진 구성 (DeviceManager ← AAutoEngine ← Platform + Config)
    aauto::core::DeviceManager device_manager;
    aauto::core::HeadunitConfig config;  // 필요 시 커스터마이징
    aauto::core::AAutoEngine engine(device_manager, platform, config);
    engine.Initialize();

    // 4. USB 디바이스 감지 시작
    auto detector = std::make_shared<aauto::hw::UsbDeviceDetector>(device_manager);
    if (!detector->Init() || !detector->Start()) {
        AA_LOG_E() << "디바이스 감지기 구동 실패";
        return -1;
    }
    AA_LOG_I() << "USB 장치를 연결하세요. (ESC 또는 Ctrl+C 로 종료)";

    // 5. 메인 스레드: 플랫폼 이벤트 루프 (SDL2: SDL_PollEvent / Qt: exec())
    platform->Run();

    // 6. 순차 종료
    detector->Stop();
    AA_LOG_I() << "=== 프로그램 종료 ===";
    return 0;
}
