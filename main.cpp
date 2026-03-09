#define LOG_TAG "Main"
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

#include "aauto/core/AAutoEngine.hpp"
#include "aauto/hw/IDeviceDetector.hpp"
#include "aauto/hw/UsbDeviceDetector.hpp"
#include "aauto/utils/Logger.hpp"

int main() {
    AA_LOG_I() << "=== Android Auto (AAuto) 엔진 초기화 ===";

    // 1. AAuto 엔진 구동 (프레임워크 라이프사이클 관리자)
    aauto::core::AAutoEngine engine;

    if (!engine.Initialize()) {
        AA_LOG_E() << "엔진 초기화 실패!";
        return -1;
    }
    AA_LOG_I() << "엔진 초기화 완료.";

    // 2. 디바이스 감지기 구동 (추상화된 인터페이스 사용)
    std::shared_ptr<aauto::hw::IDeviceDetector> detector = std::make_shared<aauto::hw::UsbDeviceDetector>();

    if (!detector->Init()) {
        AA_LOG_E() << "디바이스 감지기 초기화 실패!";
        return -1;
    }

    if (!detector->Start()) {
        AA_LOG_E() << "디바이스 감지기 구동 실패!";
        return -1;
    }

    // 3. 메인 스레드 대기 (실제 앱에서는 GUI 이벤트 루프 등이 돕니다)
    AA_LOG_I() << "USB 장치를 연결하거나 해제해 보세요. (종료하려면 'quit' 입력)";
    AA_LOG_I() << "엔터를 누르면 현재 연결된 1번째 디바이스로 가상 데이터를 전송 시도합니다.";
    while (true) {
        std::string input;
        std::getline(std::cin, input);
        if (input == "quit" || input == "q")     break;

        // AAutoEngine을 통해 세션 객체에 접근 (편의상 Engine에 SendDataTest 인터페이스를 추가할 수도 있으나, 
        // 여기서는 DeviceManager의 이벤트를 통해 상태와 데이터 흐름이 어떻게 동작하는지만 확인하도록 놔둡니다.)
        AA_LOG_D() << "AAutoEngine은 연결/해제 이벤트에 따라 백그라운드 스레드에서 메시지 처리를 진행 중입니다.";
    }

    // 안전한 종료
    detector->Stop();
    AA_LOG_I() << "=== 프로그램 종료 ===";
    return 0;
}
