#define LOG_TAG "Main"
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

#include "aauto/core/AAutoEngine.hpp"
#include "aauto/hw/IDeviceDetector.hpp"
#include "aauto/hw/UsbDeviceDetector.hpp"
#include "aauto/service/IService.hpp"
#include "aauto/video/VideoRenderer.hpp"
#include "aauto/utils/Logger.hpp"

int main() {
    AA_LOG_I() << "=== Android Auto (AAuto) 엔진 초기화 ===";

    // 1. VideoRenderer 생성 및 초기화 (SDL 메인 스레드 요구사항 충족)
    auto renderer = std::make_shared<aauto::video::VideoRenderer>();
    if (!renderer->Initialize(800, 480, "Android Auto - Video")) {
        AA_LOG_E() << "VideoRenderer 초기화 실패";
        return -1;
    }
    AA_LOG_I() << "VideoRenderer 초기화 완료";

    // 2. VideoRenderer를 ServiceFactory에 등록 (VideoService 생성 시 자동 주입)
    aauto::service::ServiceFactory::SetVideoRenderer(renderer);

    // 3. AAuto 엔진 구동
    aauto::core::AAutoEngine engine;
    if (!engine.Initialize()) {
        AA_LOG_E() << "엔진 초기화 실패!";
        return -1;
    }
    AA_LOG_I() << "엔진 초기화 완료.";

    // 4. 디바이스 감지기 구동
    std::shared_ptr<aauto::hw::IDeviceDetector> detector =
        std::make_shared<aauto::hw::UsbDeviceDetector>();

    if (!detector->Init() || !detector->Start()) {
        AA_LOG_E() << "디바이스 감지기 구동 실패!";
        return -1;
    }
    AA_LOG_I() << "USB 장치를 연결하세요. (ESC 또는 창 닫기로 종료)";

    // 5. SDL 렌더링 루프를 메인 스레드에서 실행 (SDL 요구사항)
    //    - 폰 연결 후 비디오가 시작되면 자동으로 그려집니다
    renderer->Run();

    // 6. 안전한 종료
    renderer->Stop();
    detector->Stop();
    AA_LOG_I() << "=== 프로그램 종료 ===";
    return 0;
}
